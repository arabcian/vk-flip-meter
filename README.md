# FLM — Vulkan Flip Meter / Frame Pacing Layer (v2.6)

A Vulkan implicit layer that smooths out frame delivery on Linux, with a
particular focus on **VRR panels (G-Sync/FreeSync) combined with frame
generation (DLSS-FG / FSR-FG)** on GPUs that lack hardware flip metering
(e.g. RTX 40-series). It intercepts `vkQueuePresentKHR` /
`vkAcquireNextImageKHR` and, depending on mode, either caps the frame rate
or re-times presents so frames leave the queue at more even intervals.

---

## ⚠️ Disclaimer

This project was built with AI assistance and is offered **as-is, with no
warranty of any kind**. It hooks into the Vulkan present/acquire path of
every 3D application you run it with, which means bugs here can affect
**any game or app using that path**, not just the intended target.

By building, installing, or running this layer you accept that:

- It may cause **stutter, frame-time regressions, driver instability, GPU
  hangs/resets, compositor glitches, or crashes** in some games, engines,
  or driver versions.
- Behavior can vary across GPU vendors, driver versions, DXVK/VKD3D
  versions, and MFG implementations — what works well on one setup may not
  on another.
- Live-tuning (`FLM_CONFIG` + `SIGUSR1`) and env vars are unvalidated
  power-user knobs; setting them outside sane ranges is your responsibility
  (most are clamped internally, but not all edge cases are covered).
- You are responsible for testing in your own games before relying on it,
  and for keeping backups/known-good configs.
- The authors accept **no liability** for data loss, hardware wear, lost
  play sessions, or any other damage arising from use of this software.

If you are not comfortable evaluating and accepting these risks yourself,
do not use this layer. Always test with `FLM_MODE=off` as a baseline and
compare before trusting the paced result.

---

## What problem this solves

On a VRR display, frame generation technologies (DLSS-FG, FSR-FG) insert
interpolated frames between real rendered frames. On GPUs without dedicated
hardware flip metering, those generated frames are **not evenly spaced** —
you typically get a short/short/short/long pattern (three closely-spaced
frames followed by a longer real one). Even though the average FPS looks
great, the panel displays this as visible micro-jitter, because VRR only
smooths what it's given — it can't fix an uneven arrival pattern by itself.

FLM sits between the application and the driver and re-shapes *when*
presents are submitted, without touching rendering itself, to correct this.

## What it can do for you

- **Smoother perceived motion with frame generation on VRR** — the primary
  use case. `FLM_FLOOR_PACING` spaces out generated frames instead of
  letting them leave the queue back-to-back, cutting the appearance of
  jitter at a given average FPS.
- **A dependable hard FPS cap** (`FLM_MODE=limiter`) that works on every
  driver, without requiring any extended presentation features — useful for
  thermal/power limits or for suppressing MFG jitter by capping below the
  point where it becomes visible.
- **Live tuning without restarting the game** — write to a config file and
  send `SIGUSR1`; no relaunch needed while you dial in a setting.
- **Objective before/after measurement** — CSV export of every frame's
  timing lets you A/B two runs (layer off vs on) and compare interval
  standard deviation, p99, and 1% lows instead of guessing by feel.
- **Automatic safety behavior** — pacing disables itself when the game is
  GPU-bound (so it never fights a genuinely maxed-out GPU), never touches
  FIFO/vsync-locked swapchains by default, and skips small helper/overlay
  windows automatically.

## What it will NOT do

- It does not increase your real frame rate or reduce GPU/CPU render time.
- It does not fix engine-side stutters (shader compilation, traversal
  stalls, asset streaming) — floor-pacing explicitly detects and skips
  these rather than pretending to smooth them.
- It is not a substitute for a GPU with proper hardware flip metering; it
  is a software approximation for GPUs that lack it.

---

## How it works, in brief

Two independent paths, only one of which is normally active at a time:

1. **LIMITER** — a pure local-clock FPS cap at `vkQueuePresentKHR`. Does
   not require `presentWait`; works on any driver. Always deterministic:
   given `FLM_TARGET_FPS`, frames are held to that absolute cadence.
2. **PACER** — requires `VK_KHR_present_wait`. A background measurement
   thread reads real flip timestamps, estimates the current cadence, and
   (if MFG is auto-detected) redistributes generated frames evenly across
   the flip interval. Runs only on MAILBOX/IMMEDIATE swapchains by default
   (FIFO is already vsync-locked — see `FLM_PACE_FIFO` to override).

Pacing only ever stalls at a single, configurable gate point (`present` by
default) to avoid double-latency mistakes, automatically disables itself
when the game is GPU-bound, and corrects timeline drift gradually instead
of hard-resetting it. See the comment header of `src/flip_meter.cpp` for
the full, versioned changelog of every fix and the reasoning behind it.

---

## Requirements

- Linux with the Vulkan loader and `vk_layer.h` (Gentoo:
  `media-libs/vulkan-loader`, `media-libs/vulkan-layers`).
- CMake + Ninja or Make, a C++17 compiler.
- `VK_KHR_present_wait` support in your driver/game for the PACER path
  (optional — LIMITER works everywhere).

## Building and installing

```bash
./build.sh                # installs to /usr/local by default
./build.sh /usr           # or a custom prefix
FLM_NATIVE_BUILD=ON ./build.sh   # -march=native, this machine only
```

The manifest (`VkLayer_cpu_flip_meter.json`) is generated by CMake with the
correct install path and version, and installed alongside the shared
library. Verify with:

```bash
vulkaninfo --summary | grep -i flip_meter
```

## Quick start

```bash
# Sanity check — the layer is active if you see a flat 60 FPS line:
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=limiter FLM_TARGET_FPS=60 mangohud %command%

# VRR + frame generation, the primary use case:
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=present FLM_FLOOR_PACING=1 \
  FLM_FLOOR_RATIO=850 FLM_CONFIG=/tmp/flm.conf mangohud %command%

# Hard FPS cap instead (any panel type):
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=limiter FLM_TARGET_FPS=120 %command%
```

`%command%` is the Steam launch-options placeholder; drop it and run the
game binary directly outside Steam.

### Tuning `FLM_FLOOR_RATIO` by feel

| You feel... | Change |
|---|---|
| Still micro-jittery, MFG rhythm feels off | Raise it (e.g. 850 → 950) |
| Image feels "sticky", input feels delayed | Lower it (e.g. 850 → 700) |
| Occasional one-off stutter (not steady jitter) | Not this knob — likely a real engine hitch; already skipped by the hitch guard |

### Live tuning (no restart)

```bash
echo 'FLM_FLOOR_RATIO=900' > /tmp/flm.conf
kill -SIGUSR1 $(pidof <game_binary>)
```

Confirmed applied when the log (`FLM_LOG_LEVEL=INFO`) prints the reloaded
config line.

### A/B measurement

```bash
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=off     FLM_CSV=/tmp/off.csv %command%
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=present FLM_CSV=/tmp/on.csv  %command%
```
Exclude the first 1-2 minutes (shader compilation) and compare
`interval_ns` stddev/p99 between the two CSVs.

---

## Full environment variable reference

| Variable | Purpose |
|---|---|
| `FLM_MODE=auto\|present\|limiter\|off` | `auto` (default): PACER if `presentWait` is available, else LIMITER if a target FPS is set. `present`: force PACER. `limiter`: pure FPS cap. `off`: disable everything (A/B baseline). |
| `FLM_TARGET_FPS=<n>` | Target FPS for LIMITER/PACER (0-1000, 0 = natural cadence). Setting this >0 always engages the hard-cap limiter path. |
| `FLM_PACE_POINT=present\|acquire\|both` | Which call the single pacing gate sits on (default `present`). |
| `FLM_PACE_FIFO=1` | Allow PACER/floor-pacing on FIFO/FIFO_RELAXED swapchains too (default 0/off — FIFO is already vsync-locked). |
| `FLM_FLOOR_PACING=1\|0` | VRR+MFG floor-pacing (default 1/on). Ensures a present doesn't leave earlier than a floor fraction of the slot width after the previous one. |
| `FLM_FLOOR_RATIO=850` | Floor as ratio/1000 of the slot width (500-1000). Main tuning knob — higher = tighter/flatter, lower = looser/less latency. Hot-reloadable. |
| `FLM_FLOOR_MFG_ADAPT=1` | Relax `FLOOR_RATIO` automatically as the detected MFG multiplier grows, since higher multipliers need more slack. |
| `FLM_FLOOR_MFG_STEP=40` | How much to relax the ratio per multiplier step (0-200). |
| `FLM_FLOOR_AUTOTUNE=1` | Closed-loop ratio adjustment: tightens when there's headroom, loosens quickly on signs of braking. |
| `FLM_PRESENT_LEAD_NS=1000000` | How far before the predicted flip to submit present (ns). |
| `FLM_SPIN_NS=150000` | Final spin-wait window before the gate releases (0 = pure sleep, lowest CPU use). |
| `FLM_SPIN_ADAPT=1` | Auto-adjust the spin margin from measured wakeup latency instead of using a fixed value. |
| `FLM_DRIFT_TOLERANCE_NS=0` | Timeline drift tolerance (0 = auto, interval/4). |
| `FLM_MFG_MULTIPLIER=0` | 0 = auto-detect the frame-generation multiplier, 1-4 = force it. |
| `FLM_RT_PRIORITY=0` | SCHED_FIFO priority for the measurement thread (needs `CAP_SYS_NICE`). |
| `FLM_MEASURE_CPU=0-3` | Pin the measurement thread to specific cores; accepts comma-separated ranges (e.g. `0-3,8,10-11`). |
| `FLM_STATS=1` / `FLM_STATS_INTERVAL=5` | Periodic summary log (count, average, p99, max, fake/hitch counts, MFG multiplier, pacing state). |
| `FLM_CSV=/tmp/flm.csv` | Per-frame CSV dump for offline analysis (survives swapchain recreation; appends rather than truncating). |
| `FLM_CONFIG=/tmp/flm.conf` | Live config file (`KEY=VALUE`, `#` comments); combine with `SIGUSR1` to reload without restarting. |
| `FLM_LOG_LEVEL=DEBUG\|INFO\|WARN\|ERROR` | Log verbosity. |
| `FLM_LOG_FILE=/path` | Log destination (default stderr). |
| `SIGUSR1` | Re-read the `FLM_CONFIG` file at runtime. |

---

## Troubleshooting

1. **Confirm the layer is loading**: run with `FLM_LOG_LEVEL=INFO
   FLM_LOG_FILE=/tmp/flm.log` and check for a `Config: mode=...` line.
2. **Check `presentWait` support**: if the log reports it's unsupported,
   only LIMITER is usable — PACER and floor-pacing need it.
3. **Check for FIFO**: if PACER seems inactive under `FLM_MODE=present`,
   the swapchain may be FIFO (vsync-locked); this is by design unless you
   set `FLM_PACE_FIFO=1`.
4. **Give it a moment**: the first ~30 frames are an unpaced warmup window.
5. **Measure, don't guess**: use `FLM_CSV` and compare stddev/p99 against
   an `FLM_MODE=off` baseline in the same scene.

## License

See `LICENSE`.
