# Turn-by-Turn HUD & Screen Design

Design of the on-device navigation HUD: how turn-by-turn guidance reaches the
ESP32, how it is laid out on the **round** 466×466 AMOLED, and how it composites
with the live JPEG map stream.

For the video path see [`FRAME_PIPELINE.md`](FRAME_PIPELINE.md); for the
system-wide picture see [`ARCHITECTURE.md`](ARCHITECTURE.md).

Firmware: `main/nav_hud.{h,cpp}`, `main/status_screen.{h,cpp}`,
`main/example_qspi_with_ram.cpp` (`nav_telemetry_task`, `jpeg_display_task`).
iOS sender: `MapNavSwiftUI/Streaming/NavTelemetry*.swift`.

---

## 1. Why structured telemetry instead of "baking" it into the video

The map is streamed as JPEG and drawn straight to the panel. Guidance is sent
**separately** as a tiny structured packet and rendered natively on the device,
because:

- The captured map frame is a center-crop that excludes the phone's own banner.
- Native text stays crisp; text re-encoded through JPEG at ~400 px is mushy.
- Guidance survives a stalled/disabled video stream (the HUD can stand alone).
- A telemetry packet is ~40 bytes at ~2 Hz vs. a video frame's tens of KB.

---

## 2. Transport & wire format

A second UDP data plane, independent of the latency-critical frame receiver:

```
iOS RouteControllerProgressDidChange ──► NavTelemetry (~2 Hz)
        │  UDP :5002  (NAV_TELEMETRY_PORT), unicast to discovered ESP32s
        ▼
ESP32 nav_telemetry_task (Core 0) ──► nav_hud_update() ──► latest state (mutex)
```

`nav_telemetry_task` lives on Core 0 with the other networking tasks; it only
parses packets and updates shared state — **it never draws** (see §5).

Packet — little-endian, 24-byte fixed header + two UTF-8 strings. Defined
identically in `nav_hud.h`, `NavTelemetry.swift`, and `tests/nav_telemetry_sender.py`:

| off | size | field | notes |
|----:|----:|-------|-------|
| 0 | 4 | magic `'M''N''A''V'` | rejected if mismatched |
| 4 | 1 | version | `NAV_TELEMETRY_VERSION` (1) |
| 5 | 1 | flags | bit0 navigating, bit1 rerouting, bit2 arrived, bit3 metric (km/m else mi/ft) |
| 6 | 1 | maneuverType | `nav_maneuver_t` (turn/depart/arrive/roundabout/merge/ramps/fork/continue) |
| 7 | 1 | maneuverModifier | `nav_modifier_t` (straight/slight/sharp L·R/uTurn) |
| 8 | 4 | distanceToManeuver_m | uint32 |
| 12 | 4 | distanceRemaining_m | uint32 |
| 16 | 4 | durationRemaining_s | uint32 |
| 20 | 1 | roundaboutExit | 0 = n/a |
| 21 | 1 | primaryLen | bytes |
| 22 | 1 | secondaryLen | bytes |
| 23 | 1 | reserved | 0 |
| 24 | … | primaryText, secondaryText | UTF-8, lengths above |

iOS maps the SDK's `ManeuverType`/`ManeuverDirection` to the compact byte enums.
The device derives presentation from type+modifier — a maneuver **icon** in the
band (§4) and a readable phrase ("TURN RIGHT", "ROUNDABOUT 2nd", "ARRIVE") in the
full-screen fallback — keeping the packet small.

**Freshness:** a packet is "fresh" for `NAV_HUD_FRESH_MS` (5 s) while navigating
*or* just arrived (so the arrival state can show briefly). Staleness reverts the
screen to plain video / the wait screen.

---

## 3. The round-screen constraint (the key design driver)

The panel is **physically circular**: only the inscribed circle is lit (radius
`PANEL_CENTER = 233`, centre `(233, 233)`); the four corners are dark. To stay
clear of the bezel we place UI within an inset **safe radius**
`R = PANEL_SAFE_RADIUS = 225` (`PANEL_CENTER − PANEL_SAFE_INSET`, both in
`stream_config.h`). The safe half-width at row `y` is `h = √(R² − (y − 233)²)`,
so content must fit in `x ∈ [233 − h, 233 + h]` — exactly what `safe_half_width()`
and `row_half_width()` compute.

Usable width is full across the middle and pinches to nothing toward top/bottom:

| screen row y | usable width (safe) |
|----:|----:|
| 233 (center) | 450 |
| 320 (band top) | 415 |
| 360 | 371 |
| 400 | 302 |
| 440 | 176 |
| 458 (safe edge) | 0 |

(The physically lit circle is ~16 px wider — 466 at the centre — but UI targets
the safe inset above.)

**Consequences for the UI:**

1. **Never left-align to the pixel edge.** Anything at `x≈16` on a top/bottom row
   lands in a dark corner. All HUD text is **centered on the vertical axis**.
2. **Place each line where it fits.** A line's usable width is the *narrowest*
   row its glyphs span; `row_half_width(cy, scale)` checks both the top and
   bottom of the glyph block and takes the min (so it's correct for either the
   top or the bottom cap).
3. **Truncate, don't clip.** `fit_centered()` shortens a string with a trailing
   `.` to that usable width, so long road names degrade cleanly instead of
   losing glyphs into the corners.

---

## 4. Layout — bottom HUD band + map letterbox

The map fills the **top region**; the HUD is a **band on the bottom cap**
(`NAV_HUD_BAND_HEIGHT = 146`, starting at `NAV_HUD_BAND_Y = 320`). iOS captures a
`466 × 320` frame (`STREAM_WIDTH × STREAM_HEIGHT`) to fill the region exactly.

```
        ╭───────────────────────╮  y=0
       ╱                         ╲
      │        map (466×320)      │   top region [0, 320], drawn by jpeg_display_task
       ╲                         ╱
        ┝━━━━━━━━━━━━━━━━━━━━━━━━━┥  y=320   chord separator (map/band boundary)
       ╱      ⮕   300 m          ╲      cy=32  maneuver icon (52px) + distance (scale 3)
      │     Prins Hendrikkade     │     cy=66  road  (scale 2, truncated)
       ╲      12 min  3.9 km     ╱      cy=90  ETA   (scale 2)
        ╰───────────────────────╯  y=466
```

The bottom cap is **widest at its top edge** and narrows toward `y=466`, so the
icon+distance group sits near the boundary and the shorter ETA sits as high as it
can while still fitting. Everything is centered on the panel axis; `cy` is the
canvas row within the band (screen row = `NAV_HUD_BAND_Y + cy`).

### Maneuver icons

Icons are drawn procedurally from line primitives (`draw_arrow`, `draw_uturn`,
`draw_roundabout`, `draw_flag`, `draw_reroute_icon` in `nav_hud.cpp`), so one
routine covers every turn angle (`modifier_theta`) and the set scales freely. The
full-screen fallback (`status_screen_show`, no video) still uses the text phrase
from `maneuver_phrase`.

### Display & guidance states

| Telemetry | Video | Screen |
|-----------|-------|--------|
| fresh | streaming | **Letterbox**: map (top) + HUD band (bottom) |
| fresh | none | Full-screen HUD (4 centered text lines via `status_screen_show`) |
| stale | streaming | Full-screen video (centered) |
| stale | none | "Waiting for stream" |

Within the band, three guidance states are rendered:

- **Normal** — maneuver icon + distance, road name, ETA.
- **Rerouting** (`NAV_FLAG_REROUTING`) — spinner icon + "Rerouting" (amber) + ETA.
  iOS sets the flag between the `RouteControllerWillReroute` and `…DidReroute`
  notifications.
- **Arrived** (`NAV_FLAG_ARRIVED`) — flag icon + "Arrived" (green) + destination.
  Kept "fresh" briefly so the arrival shows, then the screen reverts.

Units (km/m vs mi/ft) follow `NAV_FLAG_METRIC`, set by iOS from `useMetricUnits`.

A letterbox↔fullscreen switch triggers a one-time full clear so no stale band or
border pixels remain.

---

## 5. Rendering & the single-drawer invariant

The panel and its DMA-done semaphore are **not** thread-safe, so exactly one
task draws: `jpeg_display_task`. `nav_telemetry_task` only mutates shared state.

Per video frame, `jpeg_display_task`:

1. Reads `nav_hud_is_fresh()` → `letterbox`.
2. Computes the map's draw offset (top region when letterbox, else centered).
3. Repaints the band **only when telemetry changed** — `nav_hud_seq()` differs
   from the last painted sequence. The band region is disjoint from the map, so
   it persists between repaints (no need to redraw it every frame).
4. Decodes the JPEG; `jpeg_decode_callback` pushes each MCU straight to the panel
   at the computed offset. No framebuffer.

Because the band (`[320,466]`) and map (`[0,320]`) regions never overlap, there
is no compositing conflict and order doesn't matter for correctness.

### Shared text primitives

The 8×8 bitmap font lives once in `status_screen.cpp`. Both the full-screen
status path and the HUD band use the same primitives (declared in
`status_screen.h`): `status_be565`, `status_text_width`, `status_draw_text`
(into an arbitrary RGB565 canvas), and `status_blit` (strip-staged, DMA-synced
region blit). `nav_hud_draw_band()` renders a `466×146` PSRAM canvas and blits it
to `dst_y = NAV_HUD_BAND_Y`.

---

## 6. Tuning knobs

| Knob | Location | Effect |
|------|----------|--------|
| `PANEL_SAFE_INSET` | `stream_config.h` | px kept clear of the bezel; raise if edges clip on your panel |
| `NAV_HUD_BAND_HEIGHT` | `stream_config.h` | band size; the map height (`STREAM_HEIGHT`) is the panel minus this |
| `NAV_HUD_BAND_Y` | `stream_config.h` | band top row (bottom band = `DIAMETER − HEIGHT`) |
| `NAV_HUD_FRESH_MS` | `stream_config.h` | how long guidance shows after the last packet |
| line `cy` / scale | `nav_hud_draw_band()` | per-line vertical position and text size |

If `NAV_HUD_BAND_HEIGHT`, `NAV_HUD_BAND_Y`, or `STREAM_WIDTH/HEIGHT` change,
update iOS `NavigationFrameCapture.outputSize` to match the map region.

---

## 7. Testing without the phone

Host tools speak the same protocols, so the HUD can be exercised standalone
(pass `--esp-ip` to both so they don't both bind the discovery port):

```sh
python3 tests/nav_telemetry_sender.py --esp-ip <ESP_IP> --loop   # scripted route → HUD
python3 tests/udp_test_sender.py      --esp-ip <ESP_IP> --fps 20 # map frames → letterbox
```

`nav_telemetry_sender.py` alone exercises the full-screen HUD (no video). Running
both shows the letterbox: map on top, native band on the bottom.
