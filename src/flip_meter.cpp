#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

// Newer Vulkan SDK dropped VK_LAYER_EXPORT
#ifndef VK_LAYER_EXPORT
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#include <atomic>
#include <pthread.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Config:
//   FLM_MFG_MULTIPLIER = 1..4  (default 1)
//   FLM_TARGET_FPS     = N     (0 = auto from display timestamps)
//   FLM_VERBOSE        = 0,1
// ─────────────────────────────────────────────────────────────────────────────

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using NS        = std::chrono::nanoseconds;

static int  g_mfg_mult  = 1;
static int  g_target_fps = 0;
static bool g_verbose   = false;

static void read_config() {
    const char* e;
    if ((e = getenv("FLM_MFG_MULTIPLIER"))) g_mfg_mult   = std::max(1, std::min(4, atoi(e)));
    if ((e = getenv("FLM_TARGET_FPS")))     g_target_fps = std::max(0, atoi(e));
    if ((e = getenv("FLM_VERBOSE")))        g_verbose    = atoi(e) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Precise wait: hybrid sleep + spinloop
// Much more accurate than sleep_for alone (~0.05ms vs ~4ms jitter)
// ─────────────────────────────────────────────────────────────────────────────

static void precise_wait_until(TimePoint target) {
    // Sleep in halving steps until within 0.5ms, then spinloop
    for (;;) {
        auto now  = Clock::now();
        auto left = std::chrono::duration_cast<NS>(target - now);
        if (left.count() <= 0) return;
        if (left.count() > 500000) {                          // > 0.5ms: sleep half
            std::this_thread::sleep_for(NS(left.count() / 2));
        } else {                                               // <= 0.5ms: spin
            std::this_thread::yield();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch tables
// ─────────────────────────────────────────────────────────────────────────────

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance     DestroyInstance;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr   GetDeviceProcAddr;
    PFN_vkDestroyDevice       DestroyDevice;
    PFN_vkQueuePresentKHR     QueuePresentKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkWaitForPresentKHR   WaitForPresentKHR;   // VK_KHR_present_wait
    bool                      has_present_wait;
};

static std::mutex g_inst_lock;
static std::unordered_map<VkInstance, InstanceDispatch> g_inst_map;

static std::mutex g_dev_lock;
static std::unordered_map<VkDevice, DeviceDispatch> g_dev_map;

// ─────────────────────────────────────────────────────────────────────────────
// Per-swapchain state
// ─────────────────────────────────────────────────────────────────────────────

struct SwapchainState {
    std::mutex lock;

    // Present ID counter (for VK_KHR_present_id)
    std::atomic<uint64_t> next_present_id{1};

    // Display-side timestamps from vkWaitForPresentKHR
    // Accurate: these are when frames actually appeared on screen
    static constexpr int HIST = 8;
    NS   display_intervals[HIST] = {};
    int  di_idx   = 0;
    int  di_count = 0;
    int64_t filtered_interval_ns = 16666666LL;
    int64_t timeline_target_ns = 0;

    // Target time for next acquire (set by measurement thread)
    std::atomic<int64_t> next_target_ns{0};  // absolute Clock ns

    // Measurement thread
    std::thread       measure_thread;
    std::atomic<bool> measure_running{false};

    // Device handle for vkWaitForPresentKHR calls
    VkDevice       device    = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    DeviceDispatch* disp     = nullptr;

    // Warmup: skip pacing for first N frames
    int frame_count = 0;
    static constexpr int WARMUP = 30;

    // Hitch detection — set by measurement thread, cleared after recovery
    // When hitch is active, acquire skips pacing entirely
    std::atomic<bool> hitch_active{false};
    std::atomic<int>  hitch_recovery_frames{0};
    static constexpr int HITCH_RECOVERY = 8; // frames to skip after hitch clears
    static constexpr int64_t HITCH_THRESHOLD_NS = 40000000LL; // 40ms = below 25fps

    NS avg_display_interval() const {
        return NS(filtered_interval_ns > 0 ? filtered_interval_ns : 16666666LL);
    }

    ~SwapchainState() {
        measure_running = false;
        if (measure_thread.joinable())
            measure_thread.join();
    }
};

static std::mutex g_sc_lock;
static std::unordered_map<VkSwapchainKHR, SwapchainState*> g_sc_map;

// ─────────────────────────────────────────────────────────────────────────────
// Measurement thread — calls vkWaitForPresentKHR to get accurate display times
// ─────────────────────────────────────────────────────────────────────────────

static void measurement_thread_fn(SwapchainState* st) {
    uint64_t wait_id = 1;
    TimePoint last_display;
    bool last_valid = false;

    while (st->measure_running) {
        if (!st->disp || !st->disp->has_present_wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Block until present wait_id is on screen (1 second timeout)
        VkResult r = st->disp->WaitForPresentKHR(
            st->device, st->swapchain, wait_id, 1000000000ULL);

        if (r == VK_SUCCESS) {
            auto now = Clock::now();

            if (last_valid) {
                // Record display interval
                NS interval = std::chrono::duration_cast<NS>(now - last_display);

                std::lock_guard<std::mutex> lk(st->lock);

                // Pre-calculate target_interval and real_frame_target
                // (may be updated below if this is a real frame)
                NS target_interval;
                if (g_target_fps > 0) {
                    target_interval = NS(1000000000LL / g_target_fps);
                } else {
                    target_interval = st->avg_display_interval();
                }
                NS real_frame_target = NS(target_interval.count() * g_mfg_mult);
                TimePoint next_target = now + real_frame_target;

                // ── Fake frame filter ────────────────────────────────
                // MFG generates (g_mfg_mult - 1) fake frames between each
                // pair of real ones, so real-frame spacing is ~avg_before
                // and fake-frame spacing is ~avg_before / g_mfg_mult.
                // The split point must scale with the multiplier. Using
                //   split = avg * (1/mult + 1) / 2
                // biases toward classifying borderline samples as real
                // (safer: an undetected fake just blends into a slightly
                // wrong average, but a real frame wrongly tagged "fake"
                // is dropped from pacing entirely). This gives:
                //   2x -> split ~0.75*avg   3x -> ~0.667*avg   4x -> ~0.625*avg
                // A fixed /3 threshold (old code) put the split at 0.333*avg
                // for every multiplier — far too low for 3x/4x, where fake
                // intervals (~avg/3, ~avg/4) would land above that line and
                // get misclassified as real. With g_mfg_mult == 1 there is
                // no MFG running, so the filter is skipped — every present
                // counts as real.
                NS avg_before = st->avg_display_interval();
                bool is_fake = false;

                if (g_mfg_mult > 1 && st->di_count > 4) {
                    int64_t avg_ns   = avg_before.count();
                    int64_t split_ns = (avg_ns * (1000 / g_mfg_mult + 1000)) / 2000;
                    is_fake = (interval.count() < split_ns) ||
                              (interval > avg_before * 3);
                }

                if (is_fake) {
                    if (g_verbose)
                        fprintf(stderr,
                            "[flip_meter] id=%lu FAKE frame interval=%ldus — skipping\n",
                            (unsigned long)wait_id, interval.count() / 1000);
                    // Don't update rolling average or target for fake frames
                } else {
                    // Real frame — update rolling average and target
                    st->display_intervals[st->di_idx % SwapchainState::HIST] = interval;
                    st->di_idx++;
                    st->di_count++;

                    int64_t sample = interval.count();
                    st->filtered_interval_ns =
                        (st->filtered_interval_ns * 9 + sample) / 10;

                    if (g_target_fps > 0) {
                        target_interval = NS(1000000000LL / g_target_fps);
                    } else {
                        target_interval = NS(st->filtered_interval_ns);
                    }

                    real_frame_target = NS(target_interval.count() * g_mfg_mult);

                    int64_t now_ns =
                        std::chrono::duration_cast<NS>(now.time_since_epoch()).count();

                    if (st->timeline_target_ns == 0) {
                        st->timeline_target_ns = now_ns + real_frame_target.count();
                    } else {
                        st->timeline_target_ns += real_frame_target.count();

                        // ── Drift correction (both directions) ──────────
                        // The old code only resynced when the cumulative
                        // timeline fell BEHIND now_ns (timeline < now_ns).
                        // That catches the case where real_frame_target
                        // measurements run long, but does nothing when they
                        // run short and the timeline drifts AHEAD of now_ns
                        // — in that case acquire keeps waiting on a target
                        // that is progressively further in the future than
                        // it should be, over-throttling the render thread.
                        // Clamp drift in both directions to half a frame:
                        // small jitter still accumulates smoothly (avoids
                        // resetting every frame), but the timeline can never
                        // wander more than half a frame_target away from
                        // the actual present clock.
                        int64_t tolerance_ns = real_frame_target.count() / 2;
                        int64_t drift_ns     = st->timeline_target_ns - now_ns;

                        if (drift_ns < -tolerance_ns || drift_ns > tolerance_ns) {
                            if (g_verbose)
                                fprintf(stderr,
                                    "[flip_meter] timeline drift=%ldus exceeded tolerance, resyncing\n",
                                    drift_ns / 1000);
                            st->timeline_target_ns = now_ns + real_frame_target.count();
                        }
                    }

                    st->next_target_ns.store(st->timeline_target_ns);

                    // Hitch detection on real frames only
                    if (interval.count() > SwapchainState::HITCH_THRESHOLD_NS) {
                        st->hitch_active.store(true);
                        st->hitch_recovery_frames.store(SwapchainState::HITCH_RECOVERY);
                        if (g_verbose)
                            fprintf(stderr, "[flip_meter] HITCH detected id=%lu interval=%ldms — pausing pacing\n",
                                    (unsigned long)wait_id, interval.count() / 1000000);
                    } else if (st->hitch_active.load()) {
                        int rem = st->hitch_recovery_frames.fetch_sub(1) - 1;
                        if (rem <= 0) {
                            st->hitch_active.store(false);
                            if (g_verbose)
                                fprintf(stderr, "[flip_meter] hitch recovery complete, resuming pacing\n");
                        }
                    }

                    if (g_verbose) {
                        fprintf(stderr,
                            "[flip_meter] REAL id=%lu interval=%ldus avg=%ldus target=%ldus mfg=%dx%s\n",
                            (unsigned long)wait_id,
                            interval.count() / 1000,
                            st->avg_display_interval().count() / 1000,
                            real_frame_target.count() / 1000,
                            g_mfg_mult,
                            st->hitch_active.load() ? " [HITCH]" : "");
                    }
                }
            }

            last_display = now;
            last_valid   = true;
            wait_id++;

        } else if (r == VK_TIMEOUT) {
            // No new present in 1s — game probably paused, reset
            last_valid = false;
        } else {
            // Error or swapchain gone
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch key
// ─────────────────────────────────────────────────────────────────────────────

static inline void* dispatch_key(void* handle) {
    return *(void**)handle;
}

// ─────────────────────────────────────────────────────────────────────────────
// vkCreateInstance
// ─────────────────────────────────────────────────────────────────────────────

extern "C" {

VK_LAYER_EXPORT VkResult VKAPI_CALL
FLM_vkCreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                     const VkAllocationCallbacks* pAllocator,
                     VkInstance*                  pInstance)
{
    read_config();

    VkLayerInstanceCreateInfo* chain =
        (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain &&
           !(chain->sType    == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    auto fn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult res = fn(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceDispatch d{};
    d.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gipa(*pInstance, "vkGetInstanceProcAddr");
    d.DestroyInstance     = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");

    std::lock_guard<std::mutex> lk(g_inst_lock);
    g_inst_map[*pInstance] = d;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
FLM_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    std::lock_guard<std::mutex> lk(g_inst_lock);
    auto it = g_inst_map.find(instance);
    if (it != g_inst_map.end()) {
        it->second.DestroyInstance(instance, pAllocator);
        g_inst_map.erase(it);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// vkCreateDevice — enable VK_KHR_present_id + VK_KHR_present_wait features
// ─────────────────────────────────────────────────────────────────────────────

VK_LAYER_EXPORT VkResult VKAPI_CALL
FLM_vkCreateDevice(VkPhysicalDevice             gpu,
                   const VkDeviceCreateInfo*    pCreateInfo,
                   const VkAllocationCallbacks* pAllocator,
                   VkDevice*                    pDevice)
{
    VkLayerDeviceCreateInfo* chain =
        (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain &&
           !(chain->sType    == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
             chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    // Advance the chain AFTER we save the pointers
    // Store the next pointer so we can restore if we need to retry
    VkLayerDeviceLink* saved_layer_info = chain->u.pLayerInfo;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    // Copy existing extensions and add present_id + present_wait if missing
    std::vector<const char*> exts(
        pCreateInfo->ppEnabledExtensionNames,
        pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

    bool has_present_id   = false;
    bool has_present_wait = false;
    for (auto& e : exts) {
        if (strcmp(e, VK_KHR_PRESENT_ID_EXTENSION_NAME)   == 0) has_present_id   = true;
        if (strcmp(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0) has_present_wait = true;
    }
    if (!has_present_id)   exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    if (!has_present_wait) exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);

    // Enable presentId + presentWait features
    VkPhysicalDevicePresentIdFeaturesKHR   present_id_feat{};
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait_feat{};
    present_id_feat.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    present_id_feat.presentId     = VK_TRUE;
    present_wait_feat.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    present_wait_feat.presentWait = VK_TRUE;
    present_id_feat.pNext         = (void*)&present_wait_feat;

    VkDeviceCreateInfo ci          = *pCreateInfo;
    ci.ppEnabledExtensionNames     = exts.data();
    ci.enabledExtensionCount       = (uint32_t)exts.size();
    // Prepend our features — skip over loader structs in pNext
    present_wait_feat.pNext        = (void*)ci.pNext;
    ci.pNext                       = &present_id_feat;

    auto fn = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult res = fn(gpu, &ci, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        // Restore chain and retry with original pCreateInfo (no feature injection)
        // This ensures MangoHud and other layers still get a valid pLayerInfo
        if (g_verbose) fprintf(stderr, "[flip_meter] present_wait enable failed, retrying without\n");
        chain->u.pLayerInfo = saved_layer_info;
        chain->u.pLayerInfo = chain->u.pLayerInfo->pNext; // re-advance for clean retry
        res = fn(gpu, pCreateInfo, pAllocator, pDevice);
        if (res != VK_SUCCESS) return res;
    }

    DeviceDispatch d{};
    d.GetDeviceProcAddr   = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
    d.DestroyDevice       = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
    d.QueuePresentKHR     = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");
    d.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(*pDevice, "vkAcquireNextImageKHR");
    d.WaitForPresentKHR   = (PFN_vkWaitForPresentKHR)gdpa(*pDevice, "vkWaitForPresentKHR");
    d.has_present_wait    = (d.WaitForPresentKHR != nullptr);

    if (g_verbose)
        fprintf(stderr, "[flip_meter] vkWaitForPresentKHR: %s\n",
                d.has_present_wait ? "enabled" : "unavailable");

    std::lock_guard<std::mutex> lk(g_dev_lock);
    g_dev_map[*pDevice] = d;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
FLM_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    std::lock_guard<std::mutex> lk(g_dev_lock);
    auto it = g_dev_map.find(device);
    if (it != g_dev_map.end()) {
        it->second.DestroyDevice(device, pAllocator);
        g_dev_map.erase(it);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// vkAcquireNextImageKHR — pacing point
// Game calls this when it wants to start rendering the next frame.
// We wait here until the display-side target time calculated by the
// measurement thread. This is the right place: before rendering starts,
// not after present.
// ─────────────────────────────────────────────────────────────────────────────

VK_LAYER_EXPORT VkResult VKAPI_CALL
FLM_vkAcquireNextImageKHR(VkDevice       device,
                           VkSwapchainKHR swapchain,
                           uint64_t       timeout,
                           VkSemaphore    semaphore,
                           VkFence        fence,
                           uint32_t*      pImageIndex)
{
    DeviceDispatch* disp = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_dev_lock);
        auto it = g_dev_map.find(device);
        if (it != g_dev_map.end()) disp = &it->second;
    }

    if (disp) {
        SwapchainState* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_sc_lock);
            auto it = g_sc_map.find(swapchain);
            if (it != g_sc_map.end()) st = it->second;
        }

        if (st && st->frame_count >= SwapchainState::WARMUP) {
            // Skip pacing entirely during hitch or recovery
            bool skip = st->hitch_active.load() ||
                        st->hitch_recovery_frames.load() > 0;

            if (!skip) {
                int64_t target_ns = st->next_target_ns.load();
                if (target_ns > 0) {
                    TimePoint target{NS(target_ns)};
                    auto now = Clock::now();
                    auto left = std::chrono::duration_cast<NS>(target - now);
                    // Tight cap: 20ms max wait
                    // If we need to wait longer than that, something is wrong
                    // — bail immediately and let the game recover
                    if (left.count() > 0 && left.count() < 20000000LL) {
                        precise_wait_until(target);
                    } else if (left.count() >= 20000000LL) {
                        // Target is stale/wrong — reset it
                        st->next_target_ns.store(0);
                    }
                }
            }
        }

        if (st) st->frame_count++;
    }

    return disp
        ? disp->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex)
        : VK_ERROR_DEVICE_LOST;
}

// ─────────────────────────────────────────────────────────────────────────────
// vkQueuePresentKHR — attach VkPresentIdKHR, start measurement thread
// ─────────────────────────────────────────────────────────────────────────────

VK_LAYER_EXPORT VkResult VKAPI_CALL
FLM_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    // Find device from queue dispatch key
    DeviceDispatch* disp = nullptr;
    VkDevice        dev  = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_dev_lock);
        void* key = dispatch_key((void*)queue);
        for (auto& [d, dd] : g_dev_map) {
            if (dispatch_key((void*)d) == key) {
                disp = &dd;
                dev  = d;
                break;
            }
        }
    }
    if (!disp) return VK_ERROR_DEVICE_LOST;

    // Process each swapchain in this present
    // Build present IDs array
    std::vector<uint64_t> present_ids(pPresentInfo->swapchainCount, 0);
    bool any_id = false;

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VkSwapchainKHR sc = pPresentInfo->pSwapchains[i];
        SwapchainState* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_sc_lock);
            auto it = g_sc_map.find(sc);
            if (it == g_sc_map.end()) {
                // First present for this swapchain — create state + start thread
                auto* nst = new SwapchainState();
                nst->device    = dev;
                nst->swapchain = sc;
                nst->disp      = disp;
                g_sc_map[sc]   = nst;
                st = nst;

                if (disp->has_present_wait) {
                    nst->measure_running = true;
                    nst->measure_thread  = std::thread(measurement_thread_fn, nst);

                    // Pin measurement thread to last CPU core
                    // Keeps it away from game render threads (typically core 0-3)
                    // and away from CAS scheduler domain boundaries
                    // hardware_concurrency() returns logical CPUs (24 on 7845HX)
                    // Last logical CPU = core 23 = CCD1, away from game threads on CCD0
                    int last_core = std::max(0, (int)std::thread::hardware_concurrency() - 1);
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(last_core, &cpuset);
                    int rc = pthread_setaffinity_np(
                        nst->measure_thread.native_handle(),
                        sizeof(cpu_set_t), &cpuset);
                    if (g_verbose)
                        fprintf(stderr, "[flip_meter] measurement thread started sc=%p pinned to core %d (rc=%d)\n",
                                (void*)sc, last_core, rc);
                }
            } else {
                st = it->second;
            }
        }

        if (disp->has_present_wait) {
            present_ids[i] = st->next_present_id.fetch_add(1);
            any_id = true;
        }
    }

    // Attach VkPresentIdKHR to present chain if we have IDs
    VkPresentIdKHR present_id_info{};
    VkPresentInfoKHR modified_info = *pPresentInfo;

    if (any_id && disp->has_present_wait) {
        present_id_info.sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        present_id_info.swapchainCount = pPresentInfo->swapchainCount;
        present_id_info.pPresentIds    = present_ids.data();
        present_id_info.pNext          = pPresentInfo->pNext;
        modified_info.pNext            = &present_id_info;
    }

    return disp->QueuePresentKHR(queue, &modified_info);
}

// ─────────────────────────────────────────────────────────────────────────────
// vkGetDeviceProcAddr / vkGetInstanceProcAddr
// ─────────────────────────────────────────────────────────────────────────────

#define INTERCEPT(fn) if (strcmp(pName, "vk" #fn) == 0) return (PFN_vkVoidFunction)FLM_vk##fn

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
FLM_vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    INTERCEPT(GetDeviceProcAddr);
    INTERCEPT(DestroyDevice);
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(AcquireNextImageKHR);

    std::lock_guard<std::mutex> lk(g_dev_lock);
    auto it = g_dev_map.find(device);
    if (it == g_dev_map.end()) return nullptr;
    return it->second.GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
FLM_vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    INTERCEPT(GetInstanceProcAddr);
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(GetDeviceProcAddr);

    if (instance == VK_NULL_HANDLE) return nullptr;
    std::lock_guard<std::mutex> lk(g_inst_lock);
    auto it = g_inst_map.find(instance);
    if (it == g_inst_map.end()) return nullptr;
    return it->second.GetInstanceProcAddr(instance, pName);
}

#undef INTERCEPT

} // extern "C"
