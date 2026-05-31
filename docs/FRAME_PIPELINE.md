# Frame Receive & Process Pipeline

Design of the path that turns a stream of UDP packets from the iOS app into pixels on the AMOLED panel. This is the deep dive; for the system-wide picture see [`ARCHITECTURE.md`](ARCHITECTURE.md).

All code is in `main/example_qspi_with_ram.cpp` unless noted.

---

## 1. Stages at a glance

```
 iOS sender ──UDP :5000──►  udp_receiver_task  ──s_ready_q──►  jpeg_display_task  ──QSPI/DMA──►  AMOLED
 (JPEG frames,             (Core 1, prio 6)    (index+size)   (Core 1, prio 5)    (per MCU)     466×466
  packetized)              assemble into pool                 decode + draw

                      ◄──────────────── s_free_q (recycled buffer indices) ◄────────────────
```

Two FreeRTOS tasks, both pinned to **Core 1** (isolated from NimBLE on Core 0 — see ARCHITECTURE §4), connected by a **frame pool** and **two index queues**. The receiver is the producer, the display task is the consumer, and buffers circulate between them with no per-frame allocation.

---

## 2. Wire protocol

The sender (`UDPStreamSender` on iOS) splits each JPEG frame into UDP datagrams on port **5000**:

| Packet | Bytes | Role |
|--------|-------|------|
| **SOI** | `0xFF 0xD8` (exactly 2) | Frame-start **delimiter** |
| **DATA** | raw JPEG, ≤1400 B each | Frame payload |
| **EOI** | `0xFF 0xD9` (exactly 2) | Frame-end **delimiter** |

Key subtlety: the 2-byte SOI/EOI packets are **framing markers only**. Each DATA payload is already a complete JPEG bitstream that contains its *own* `FFD8…FFD9`. The receiver therefore uses the marker packets purely to know where a frame begins and ends — it does **not** append them to the buffer.

This delimiter-based framing is simple but **lossy-fragile**: if a marker packet is dropped (e.g. during NimBLE preemption), frame boundaries blur. §6 covers how the receiver detects and recovers from that.

---

## 3. The frame pool (the core of the design)

Earlier revisions did `heap_caps_malloc` + `memcpy` + `free` for every frame at ~20 fps, in internal RAM. That fragmented the heap, was non-deterministic, and competed with WiFi/BLE for scarce internal SRAM — a leading cause of crashes. The pipeline now uses a **fixed pool with index-based recycling**.

### Structures

```c
#define FRAME_POOL_COUNT  3
typedef struct { int idx; size_t size; } frame_msg_t;

static uint8_t      *s_frame_pool[FRAME_POOL_COUNT];  // PSRAM buffers
static QueueHandle_t s_free_q;    // queue of int          — buffers free to fill
static QueueHandle_t s_ready_q;   // queue of frame_msg_t   — filled, ready to show
```

- Pool buffers are allocated **once** in `app_main` from **PSRAM** (`MALLOC_CAP_SPIRAM`), each `MAX_JPEG_FRAME_SIZE` (128 KB). Total `3 × 128 KB = 384 KB`, all in PSRAM → ~300 KB of internal SRAM reclaimed vs. the old design.
- `s_free_q` is seeded with all `FRAME_POOL_COUNT` indices at startup.

### Ownership flow

The **buffer index is the ownership token** — exactly one stage owns a buffer at any moment:

```
        ┌──────────────────────── s_free_q ◄───────────────────────┐
        │                                                           │
        ▼                                                           │
   receiver pops idx ─► assembles into s_frame_pool[idx] ─► pushes {idx,size}
        (at SOI)              (DATA packets)                    (at EOI)
                                                                    │
                                                                    ▼
                                                                s_ready_q
                                                                    │
                                                                    ▼
                                        display pops {idx,size} ─► decodes pool[idx]
                                                                    │
                                                                    └─► pushes idx back ─► s_free_q
```

No `malloc`/`free`, no `memcpy` of the whole frame (the receiver assembles *directly* into the pool buffer — zero-copy), and a fixed, deterministic memory footprint.

### Why N = 3

One buffer in each of: being decoded, sitting in `s_ready_q`, being filled. With `N = 2` the steady state is "one decoding + one queued," leaving the receiver **zero** free buffers whenever display rate ≈ network rate — it would drop ~every other frame. `N = 3` gives one slot of slack.

---

## 4. Receiver state machine (`udp_receiver_task`)

Two states drive frame assembly:

```
                 ┌────────────────────────────────────────────────┐
                 │                  WAITING_SOI                    │
                 │  recv 2B 0xFFD8 ─► pop s_free_q                 │
                 │     • got buffer  → cur_idx, RECEIVING_DATA     │
                 │     • none free   → drop frame, stay here       │
                 └───────────────┬────────────────────────────────┘
                                 │ SOI + buffer acquired
                                 ▼
                 ┌────────────────────────────────────────────────┐
                 │                RECEIVING_DATA                   │
                 │  DATA pkt   ─► memcpy into pool[cur_idx]        │
                 │  2B 0xFFD8  ─► mid-frame SOI: reset, keep buf   │
                 │  2B 0xFFD9  ─► publish {cur_idx,size}→s_ready_q │
                 │  overflow   ─► RX_RECYCLE, WAITING_SOI          │
                 │  500ms idle ─► RX_RECYCLE, WAITING_SOI          │
                 └────────────────────────────────────────────────┘
```

Receive loop specifics:
- Socket has `SO_RCVBUF = 64 KB` and a 1 s `SO_RCVTIMEO`. The timeout lets the task notice a **stalled assembly** (no DATA for 500 ms → reset) even when no packets arrive.
- DATA is appended into `s_frame_pool[cur_idx]` with a bounds check against `MAX_JPEG_FRAME_SIZE`.

### The one rule that keeps the pool alive: `RX_RECYCLE()`

```c
#define RX_RECYCLE() do { \
    if (cur_idx >= 0) { xQueueSend(s_free_q, &cur_idx, 0); cur_idx = -1; frame_buf = NULL; } \
} while (0)
```

A borrowed buffer **must** be returned on *every* path that abandons a frame, or the pool slowly drains until the receiver can never acquire a buffer and the stream silently dies. The recycle points are:

| Path | Why |
|------|-----|
| Frame overflow (`> MAX_JPEG_FRAME_SIZE`) | merged/oversized frame discarded |
| `s_ready_q` full | display behind; drop newest, keep the buffer |
| 500 ms assembly timeout | lost EOI; abandon partial frame |
| `recvfrom` error → socket rebind | abandon in-flight frame before reset |
| Empty frame at EOI | nothing assembled; return the buffer |

(The **mid-frame SOI** case is the exception: it keeps the same buffer and just resets `frame_used`, because a fresh frame is starting in the buffer it already holds.)

---

## 5. Decode & display stage (`jpeg_display_task`)

1. **Receive** a `frame_msg_t` from `s_ready_q` with a 3 s timeout. On timeout it repaints the wait/status screen (see [status_screen](../main/status_screen.cpp)) so the panel is never blank when the stream stalls.
2. **Validate framing (P3):** before decoding, confirm the buffer starts `0xFFD8` and ends `0xFFD9`. This cheap check protects `JPEGDEC` from faulting on a corrupt/merged buffer that slipped through.
3. **Decode:** `jpeg_dec.openRAM(pool[idx], size, jpeg_decode_callback)`, pixel type `RGB565_BIG_ENDIAN`, `decode(0,0, JPEG_USES_DMA)`.
4. **Draw per MCU:** `jpeg_decode_callback` pushes each decoded MCU block straight to the panel via `esp_lcd_panel_draw_bitmap` — no framebuffer.
5. **Recycle:** push `idx` back to `s_free_q` (replaces the old `free()`).

### DMA back-pressure

`lcd_trans_done_callback` (an ISR) gives `lcd_trans_done_sem` when each QSPI DMA transfer completes. The decode callback **takes** the semaphore before issuing the next block, so decode never outruns the panel and the source buffer isn't reused mid-transfer. This serializes decode with transfer today; overlapping them is a future optimization (the `lcd_draw_buffer[2]` scaffolding exists but is disabled).

---

## 6. Back-pressure & loss handling

The pipeline favors **freshness over completeness** — correct for a live view:

- **Display behind:** receiver finds `s_free_q` empty at SOI (or `s_ready_q` full at EOI) → it **drops the frame** and never blocks the socket. Blocking the receiver would stall the UDP socket and overflow the lwIP mailbox.
- **Lost EOI** → frames would merge; caught by the **mid-frame SOI guard** (next SOI resets the buffer) or the **500 ms timeout**, then the buffer is recycled.
- **Lost EOI *and* SOI** → DATA accumulates past 128 KB → **overflow** path discards and recycles.
- **Corrupt/merged frame under 128 KB** → caught by the **SOI/EOI validation** in the display task before decode.

These are why "frame overflow" is a *symptom of packet loss*, not a fatal condition — every path recovers without leaking a buffer or feeding garbage to the decoder.

---

## 7. Concurrency & placement

| Element | Where | Notes |
|---------|-------|-------|
| `udp_receiver_task` | Core 1, prio 6 | above decoder so packets drain first |
| `jpeg_display_task` | Core 1, prio 5 | decode + draw |
| `s_frame_pool[]` | PSRAM | 3 × 128 KB, allocated once |
| `s_free_q` / `s_ready_q` | internal RAM | small index queues, depth `FRAME_POOL_COUNT` |
| lwIP UDP mailbox | — | `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` for burst absorption |

Both tasks are on Core 1 specifically to stay off NimBLE's scheduling on Core 0 (priority-21 BLE host preemption was dropping EOI packets — see ARCHITECTURE §4).

---

## 8. Tuning knobs

| Knob | Location | Effect |
|------|----------|--------|
| `FRAME_POOL_COUNT` | `example_qspi_with_ram.cpp` | pipeline depth vs. PSRAM use (3 = 384 KB) |
| `MAX_JPEG_FRAME_SIZE` | `stream_config.h` | per-buffer size; raise only if frames legitimately exceed it |
| `FRAME_TIMEOUT_TICKS` (500 ms) | receiver | how fast a stalled assembly resets |
| ready-queue receive timeout (3 s) | display | how long before the wait screen repaints |
| `SO_RCVBUF` (64 KB) | receiver | socket burst absorption |

---

## 9. Failure modes & expected behavior

| Situation | Behavior |
|-----------|----------|
| Packet loss (occasional) | affected frame dropped; next frame displays normally |
| Display slower than network | receiver drops frames to stay current; no memory growth |
| Sustained stall / no stream | wait screen shown after 3 s; resumes on next valid frame |
| Corrupt JPEG | skipped by SOI/EOI check or `openRAM` failure; buffer recycled |
| PSRAM pool alloc fails at boot | logged + `app_main` returns (fatal, by design) |

The invariant that makes all of this safe: **the number of buffers in `s_free_q` + `s_ready_q` + in-flight (one per task) is always `FRAME_POOL_COUNT`.** Every code path either holds a buffer briefly and passes it on, or recycles it.
