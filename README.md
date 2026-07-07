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

# Spin threshold tuning:
ENABLE_LAYER_cpu_flip_meter=1 FLM_SPIN_THRESHOLD_NS=20000 %command%

# Runtime reload (oyun açıkken):
kill -SIGUSR1 $(pgrep -f game_executable)
