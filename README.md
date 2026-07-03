# vk_flip_meter

`vk_flip_meter` is a high-precision Frame Pacing and Cadence Modulation Vulkan Implicit Layer developed for Vulkan-based games and applications on Linux systems. 

Specifically tailored to eliminate micro-stuttering during Motion Frame Generation (MFG) scenarios, it perfectly aligns frame pacing while minimizing CPU and locking overhead[cite: 1].

## 🚀 Key Features

* **Precise Hybrid Wait:** Uses the POSIX `clock_nanosleep` API, which wakes up the Linux scheduler much more precisely than standard thread sleeping[cite: 1]. When approaching target microsecond thresholds, it switches to architecture-specific processor pausing (`_mm_pause` or thread yield) to achieve nanosecond-level precision[cite: 1].
* **Zero-Overhead Lockless Loop:** All mutex locks have been completely removed from the critical render loop state (`SwapchainState`)[cite: 1]. It relies entirely on `std::atomic` variables and cache-line alignment (`alignas(64)`) to ensure thread safety with zero locking overhead[cite: 1].
* **MFG Cadence Modulation:** Automatically detects modulated or "fake" frames when frame generation multipliers (from 1x up to 4x) are active, dynamically adjusting the render queue to preserve fluid smoothness[cite: 1].
* **Asynchronous Measurement & Thread Affinity:** Timing measurements run on a dedicated async thread so they do not block the main rendering thread[cite: 1]. This thread is bound to a specific core (typically the second-to-last CPU core via `hardware_concurrency() - 2`) to maintain cache locality[cite: 1].
* **Real-Time Scheduling:** Supports real-time scheduling capability using the `SCHED_FIFO` protocol, allowing the measurement thread to be prioritized at the operating system level[cite: 1].
* **Intelligent Hitch Detection & Recovery:** Automatically detects sudden system bottlenecks or large frame drops (`hitch`) and temporarily bypasses pacing mechanisms to prevent latency from building up further[cite: 1].

---

## 🛠️ Requirements and Installation

The project is heavily optimized for performance-oriented distributions like Gentoo Linux, injecting native CPU flags (`-march=native`) and aggressive optimizations (`-O3`, `LTO`).

### Dependencies
Before starting the compilation, ensure the following packages are installed on your system:
* CMake (>= 3.10)
* Ninja Build System
* Vulkan Loader & Headers (`media-libs/vulkan-loader` and `media-libs/vulkan-layers` on Gentoo)
* GCC / Clang (with C++17 support)

### Building and Installing

You can complete the installation by making the `build.sh` script executable and running it from the root directory:

```bash
chmod +x build.sh
./build.sh /usr/local