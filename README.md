# vk_flip_meter

`vk_flip_meter` is a high‑precision Frame Pacing and Cadence Modulation Vulkan Implicit Layer developed for Vulkan‑based games and applications on Linux systems.

It eliminates micro‑stuttering during Motion Frame Generation (MFG) scenarios by precisely aligning frame pacing while minimising CPU overhead.  
This version is rewritten in **C++20** with robust thread safety, RAII, and full Vulkan object lifecycle management.

## 🚀 Key Features

- **Precise Hybrid Wait** – Uses `clock_nanosleep` with absolute time for accurate sleeps, then falls back to `sched_yield` and architecture‑specific `_mm_pause` for sub‑millisecond precision.
- **Zero‑Overhead Lockless Loop** – Critical paths use `std::atomic` and cache‑line alignment (`alignas(64)`); no mutex locks in the render loop.
- **MFG Cadence Modulation** – Detects “fake” frames when MFG multipliers (1×–4×) are active and dynamically adjusts the queue.
- **Asynchronous Measurement** – Dedicated `std::jthread` with `std::stop_token` measures presentation times without blocking the main thread.
- **CPU Affinity & Real‑Time Scheduling** – Binds the measurement thread to a safe core (`cores‑2`) and optionally enables `SCHED_FIFO` via `FLM_RT_PRIORITY` environment variable.
- **Intelligent Hitch Detection** – Automatically detects large frame drops (hitches) and temporarily disables pacing to prevent latency build‑up.
- **Full Lifecycle Safety** – Tracks swapchains with `std::shared_ptr`; measurement thread is guaranteed to stop before swapchain destruction.

## 🛠️ Requirements

- **CMake** ≥ 3.20
- **Ninja** (recommended) or Make
- **Vulkan Loader & Headers** (`media-libs/vulkan-loader` and `media-libs/vulkan-layers` on Gentoo)
- **C++20** compiler (GCC 10+ or Clang 12+)

## 📦 Building and Installing

Run the provided `build.sh` script:

```bash
chmod +x build.sh
./build.sh [INSTALL_PREFIX]

# MFG autodetect (0 = otomatik):
ENABLE_LAYER_cpu_flip_meter=1 FLM_MFG_MULTIPLIER=0 %command%

# Spin window tuning (env adı FLM_SPIN_NS):
ENABLE_LAYER_cpu_flip_meter=1 FLM_SPIN_NS=20000 %command%

# Canlı ayar (oyun açıkken) — FLM_CONFIG dosyası + SIGUSR1:
ENABLE_LAYER_cpu_flip_meter=1 FLM_CONFIG=/tmp/flm.conf %command%
echo 'FLM_TARGET_FPS=90' > /tmp/flm.conf
kill -SIGUSR1 $(pgrep -f game_executable)
```

## v2.1 changes

[FIX-16] Slot interval is now calculated as a 12-frame moving window average of all present intervals (~ T/m). In v2, a fake-filtered EMA (~ T) was used; when MFG was active, it would lower the pacer's output FPS by the multiplication factor.
[FIX-17] Fixed MFG autodetection (threshold is based on the slot average, interval < 0.7 * mean); detection is frozen while the gate is active to prevent oscillations.
[FIX-18] The GPU-bound watchdog now runs only when FLM_TARGET_FPS > 0 and operates via the slot average — ensuring that MFG's bimodal intervals do not falsely trigger the watchdog.
[FIX-19] Fixed an issue where large hitches were incorrectly treated as "is_fake" and escaped hitch detection.
[FIX-20] Gate timeout ceiling is now relative to the interval: fixed a bug where the limiter would silently deactivate at targets where FLM_TARGET_FPS <= 50.
[FIX-21] On-the-fly configuration is now fully functional: FLM_CONFIG=<file> (KEY=VALUE) SIGUSR1. The signal handler is async-signal-safe (utilizes an atomic flag only).
[FIX-22] Intercepted vkAcquireNextImage2KHR; the layer now successfully engages in engines utilizing this path.
