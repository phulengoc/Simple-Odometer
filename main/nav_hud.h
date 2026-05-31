#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Turn-by-turn HUD state.
//
// Holds the latest navigation telemetry pushed by the MapNav iOS app over UDP
// (port NAV_TELEMETRY_PORT). The telemetry task parses packets and calls
// nav_hud_update(); the display task reads the state with nav_hud_format_lines()
// and paints it via status_screen_show().
//
// Threading: nav_hud_update() (telemetry task, Core 0) and the readers
// (display task, Core 1) are serialised by an internal mutex. nav_hud does NOT
// touch the panel — only the display task draws, preserving the single-drawer
// invariant the panel/DMA semaphore relies on.
// ─────────────────────────────────────────────────────────────────────────────

// Wire format (little-endian) — must match iOS NavTelemetry.swift and the
// Python test sender. 24-byte fixed header followed by two UTF-8 strings.
//
//   off sz  field
//   0   4   magic 'M','N','A','V'
//   4   1   version (= NAV_TELEMETRY_VERSION)
//   5   1   flags (NAV_FLAG_*)
//   6   1   maneuverType (nav_maneuver_t)
//   7   1   maneuverModifier (nav_modifier_t)
//   8   4   distanceToManeuver_m  (uint32)
//   12  4   distanceRemaining_m   (uint32)
//   16  4   durationRemaining_s   (uint32)
//   20  1   roundaboutExit (0 = n/a)
//   21  1   primaryLen
//   22  1   secondaryLen
//   23  1   reserved (0)
//   24  ..  primaryText[primaryLen], secondaryText[secondaryLen]
#define NAV_TELEMETRY_MAGIC0   'M'
#define NAV_TELEMETRY_MAGIC1   'N'
#define NAV_TELEMETRY_MAGIC2   'A'
#define NAV_TELEMETRY_MAGIC3   'V'
#define NAV_TELEMETRY_VERSION  1
#define NAV_TELEMETRY_HEADER   24
#define NAV_HUD_TEXT_MAX       48   // max chars kept per string (incl. NUL)

// flags bitmask
#define NAV_FLAG_NAVIGATING    0x01
#define NAV_FLAG_REROUTING     0x02
#define NAV_FLAG_ARRIVED       0x04
#define NAV_FLAG_METRIC        0x08   // distances in km/m (else mi/ft)

// Maneuver family — mirrors a compacted MapboxDirections ManeuverType.
typedef enum {
    NAV_MANEUVER_TURN       = 0,
    NAV_MANEUVER_DEPART     = 1,
    NAV_MANEUVER_ARRIVE     = 2,
    NAV_MANEUVER_ROUNDABOUT = 3,
    NAV_MANEUVER_MERGE      = 4,
    NAV_MANEUVER_ON_RAMP    = 5,
    NAV_MANEUVER_OFF_RAMP   = 6,
    NAV_MANEUVER_FORK       = 7,
    NAV_MANEUVER_CONTINUE   = 8,
} nav_maneuver_t;

// Maneuver direction — mirrors MapboxDirections ManeuverDirection.
typedef enum {
    NAV_MOD_STRAIGHT     = 0,
    NAV_MOD_SLIGHT_RIGHT = 1,
    NAV_MOD_RIGHT        = 2,
    NAV_MOD_SHARP_RIGHT  = 3,
    NAV_MOD_SLIGHT_LEFT  = 4,
    NAV_MOD_LEFT         = 5,
    NAV_MOD_SHARP_LEFT   = 6,
    NAV_MOD_UTURN        = 7,
} nav_modifier_t;

/// Create the internal mutex. Call once from app_main() before the telemetry
/// task or display task run.
void nav_hud_init(void);

/// Parse a raw UDP telemetry datagram and store it as the latest state.
/// Returns true if the packet was a valid MNAV frame. Safe to call from the
/// telemetry task only; never draws.
bool nav_hud_update(const uint8_t *pkt, size_t len);

/// True if a valid packet arrived within the last `max_age_ms` and navigation
/// is active OR has just arrived (so the arrival state can be shown briefly).
/// The display task uses this to decide whether to paint the HUD instead of the
/// "waiting for stream" screen.
bool nav_hud_is_fresh(uint32_t max_age_ms);

/// Monotonic counter incremented on every accepted telemetry packet (0 = none).
/// The display task uses it to repaint the HUD band only when state changed.
uint32_t nav_hud_seq(void);

/// Render the turn-by-turn HUD band (bottom circular cap): a maneuver icon +
/// distance, road name and ETA, with distinct reroute/arrival states. Units
/// (metric/imperial) come from the telemetry's NAV_FLAG_METRIC. Uses the shared
/// status_screen primitives and blits via status_blit(), so status_screen_init()
/// must have run. Display task only (single drawer).
void nav_hud_draw_band(void);

/// Format the latest telemetry into four display lines (title + 3 body lines),
/// matching the status_screen_show() contract:
///   l1 = distance to next maneuver (large)   e.g. "300 m"
///   l2 = maneuver instruction                 e.g. "TURN RIGHT"
///   l3 = primary road name                    e.g. "Main Street"
///   l4 = ETA summary                          e.g. "12 min  4.2 km"
/// Each buffer must hold at least NAV_HUD_TEXT_MAX bytes. Units come from the
/// telemetry's NAV_FLAG_METRIC flag.
void nav_hud_format_lines(char *l1, char *l2, char *l3, char *l4, size_t each);

#ifdef __cplusplus
}
#endif
