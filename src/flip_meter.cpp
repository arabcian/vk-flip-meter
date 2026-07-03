#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#ifndef VK_LAYER_EXPORT
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <time.h> // clock_nanosleep için eklendi

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define FLM_CPU_PAUSE() _mm_pause()
#else
#  define FLM_CPU_PAUSE() std::this_thread::yield()
#endif

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using NS        = std::chrono::nanoseconds;

static int      g_mfg_mult          = 1;
static int      g_target_fps        = 0;
static bool     g_verbose           = false;
static int64_t  g_spin_threshold_ns = 200000LL;
static int      g_rt_priority       = 0;
static int64_t  g_drift_tolerance_ns= 2000000LL; // Dinamik drift toleransı eklendi

static void read_config() {
    const char* e;
    if ((e = getenv("FLM_MFG_MULTIPLIER")))      g_mfg_mult           = std::max(1, std::min(4, atoi(e)));
    if ((e = getenv("FLM_TARGET_FPS")))          g_target_fps         = std::max(0, atoi(e));
    if ((e = getenv("FLM_VERBOSE")))             g_verbose            = atoi(e) != 0;
    if ((e = getenv("FLM_SPIN_THRESHOLD_NS")))   g_spin_threshold_ns  = std::max<int64_t>(0, std::min<int64_t>(atoll(e), 5000000LL));
    if ((e = getenv("FLM_RT_PRIORITY")))         g_rt_priority        = std::max(0, std::min(99, atoi(e)));
    if ((e = getenv("FLM_DRIFT_TOLERANCE_NS")))  g_drift_tolerance_ns = std::max<int64_t>(0, atoll(e)); // Ortam değişkenine bağlandı
}

// Hibrid hassas bekleme (POSIX clock_nanosleep ile optimize edildi)
static void precise_wait_until(TimePoint target) {
    const int64_t threshold = g_spin_threshold_ns;
    for (;;) {
        auto now  = Clock::now();
        auto left = std::chrono::duration_cast<NS>(target - now);
        if (left.count() <= 0) return;
        if (left.count() > threshold) {
            // std::this_thread::sleep_for yerine Linux scheduler'ı çok daha hassas uyandıran POSIX API'si
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = left.count() / 2;
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        } else {
            FLM_CPU_PAUSE();
        }
    }
}

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance     DestroyInstance;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr   GetDeviceProcAddr;
    PFN_vkDestroyDevice       DestroyDevice;
    PFN_vkQueuePresentKHR     QueuePresentKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkWaitForPresentKHR   WaitForPresentKHR;
    bool                      has_present_wait;
};

static std::mutex g_inst_lock;
static std::unordered_map<VkInstance, InstanceDispatch> g_inst_map;

static std::mutex g_dev_lock;
static std::unordered_map<VkDevice, DeviceDispatch> g_dev_map;

static std::mutex g_queue_lock;
static std::unordered_map<VkQueue, std::pair<VkDevice, DeviceDispatch*>> g_queue_map;

struct SwapchainState {
    // std::mutex lock; (Tamamen kaldırıldı, kilit maliyeti sıfırlandı)
    std::atomic<uint64_t> next_present_id{1};

    static constexpr int HIST = 8;
    NS   display_intervals[HIST] = {};
    int  di_idx   = 0;
    int  di_count = 0;
    int64_t filtered_interval_ns = 16666666LL;
    int64_t timeline_target_ns = 0;

    alignas(64) std::atomic<int64_t> next_target_ns{0};
    alignas(64) std::atomic<int64_t> next_present_target_ns{0};

    std::thread       measure_thread;
    std::atomic<bool> measure_running{false};

    VkDevice       device    = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    DeviceDispatch* disp     = nullptr;

    int frame_count = 0;
    static constexpr int WARMUP = 30;

    alignas(64) std::atomic<bool> hitch_active{false};
    alignas(64) std::atomic<int>  hitch_recovery_frames{0};
    static constexpr int HITCH_RECOVERY = 8;

    static constexpr int64_t HITCH_MIN_NS = 15000000LL;
    static constexpr int64_t HITCH_MAX_NS = 45000000LL;
    static constexpr int     HITCH_MULT_NUM = 3;
    static constexpr int     HITCH_MULT_DEN = 2;

    int64_t hitch_threshold_ns(int64_t avg_ns) const {
        int64_t adaptive = (avg_ns * HITCH_MULT_NUM) / HITCH_MULT_DEN;
        return std::max(HITCH_MIN_NS, std::min(adaptive, HITCH_MAX_NS));
    }

    NS median_interval() const {
        int n = std::min(di_count, HIST);
        if (n == 0) return avg_display_interval();
        NS tmp[HIST];
        std::copy(display_intervals, display_intervals + n, tmp);
        std::sort(tmp, tmp + n);
        return tmp[n / 2];
    }

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

static void measurement_thread_fn(SwapchainState* st) {
    uint64_t wait_id = 1;
    TimePoint last_display;
    bool last_valid = false;

    while (st->measure_running) {
        if (!st->disp || !st->disp->has_present_wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        VkResult r = st->disp->WaitForPresentKHR(st->device, st->swapchain, wait_id, 1000000000ULL);

        if (r == VK_SUCCESS) {
            auto now = Clock::now();

            if (last_valid) {
                NS interval = std::chrono::duration_cast<NS>(now - last_display);

                // std::lock_guard<std::mutex> lk(st->lock); satırı kaldırıldı, gereksiz overhead yok.

                NS target_interval = (g_target_fps > 0) ? NS(1000000000LL / g_target_fps) : st->avg_display_interval();
                NS real_frame_target = NS(target_interval.count() * g_mfg_mult);

                NS avg_before = st->median_interval();
                bool is_fake = false;

                if (g_mfg_mult > 1 && st->di_count > 4) {
                    int64_t avg_ns = avg_before.count();
                    int64_t split_ns = (avg_ns * 4) / 10;
                    is_fake = (interval.count() < split_ns) || (interval > avg_before * 2.5);
                }

                if (is_fake) {
                    int64_t current_target = st->next_target_ns.load();
                    st->next_target_ns.store(current_target - (real_frame_target.count() / (g_mfg_mult * 2)));

                    if (g_verbose)
                        fprintf(stderr, "[flip_meter] id=%lu FAKE Modulated Cadence — adjusting queue\n", (unsigned long)wait_id);
                } else {
                    st->display_intervals[st->di_idx % SwapchainState::HIST] = interval;
                    st->di_idx++;
                    st->di_count++;

                    int64_t sample = interval.count();
                    st->filtered_interval_ns = (st->di_count < 30) ?
                    (st->filtered_interval_ns * 6 + sample * 4) / 10 :
                    (st->filtered_interval_ns * 9 + sample) / 10;

                    target_interval = (g_target_fps > 0) ? NS(1000000000LL / g_target_fps) : NS(st->filtered_interval_ns);
                    real_frame_target = NS(target_interval.count() * g_mfg_mult);

                    int64_t now_ns = std::chrono::duration_cast<NS>(now.time_since_epoch()).count();

                    if (st->timeline_target_ns == 0) {
                        st->timeline_target_ns = now_ns + real_frame_target.count();
                    } else {
                        st->timeline_target_ns += real_frame_target.count();

                        int64_t tolerance_ns = g_drift_tolerance_ns; // Dinamik tolerans kullanılıyor
                        int64_t drift_ns = st->timeline_target_ns - now_ns;

                        if (drift_ns < -tolerance_ns || drift_ns > tolerance_ns) {
                            st->timeline_target_ns = now_ns + real_frame_target.count();
                        }
                    }

                    st->next_target_ns.store(st->timeline_target_ns);
                    st->next_present_target_ns.store(st->timeline_target_ns + (real_frame_target.count() / g_mfg_mult));

                    if (interval.count() > st->hitch_threshold_ns(avg_before.count())) {
                        st->hitch_active.store(true);
                        st->hitch_recovery_frames.store(SwapchainState::HITCH_RECOVERY);
                    } else if (st->hitch_active.load()) {
                        int rem = st->hitch_recovery_frames.fetch_sub(1) - 1;
                        if (rem <= 0) st->hitch_active.store(false);
                    }
                }
            }

            last_display = now;
            last_valid   = true;
            wait_id++;
        } else if (r == VK_TIMEOUT) {
            last_valid = false;
        } else {
            break;
        }
    }
}

static inline void* dispatch_key(void* handle) {
    return *(void**)handle;
}

extern "C" {

    VK_LAYER_EXPORT VkResult VKAPI_CALL
    FLM_vkCreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                         const VkAllocationCallbacks* pAllocator,
                         VkInstance*                  pInstance)
    {
        read_config();

        VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
        while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
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

    VK_LAYER_EXPORT VkResult VKAPI_CALL
    FLM_vkCreateDevice(VkPhysicalDevice             gpu,
                       const VkDeviceCreateInfo*    pCreateInfo,
                       const VkAllocationCallbacks* pAllocator,
                       VkDevice*                    pDevice)
    {
        VkLayerDeviceCreateInfo* chain = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
        while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
            chain = (VkLayerDeviceCreateInfo*)chain->pNext;
        if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

        PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        VkLayerDeviceLink* saved_layer_info = chain->u.pLayerInfo;
        chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

        std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
        bool has_present_id = false, has_present_wait = false;
        for (auto& e : exts) {
            if (strcmp(e, VK_KHR_PRESENT_ID_EXTENSION_NAME)   == 0) has_present_id   = true;
            if (strcmp(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0) has_present_wait = true;
        }
        if (!has_present_id)   exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        if (!has_present_wait) exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);

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
        present_wait_feat.pNext        = (void*)ci.pNext;
        ci.pNext                       = &present_id_feat;

        auto fn = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
        VkResult res = fn(gpu, &ci, pAllocator, pDevice);
        if (res != VK_SUCCESS) {
            chain->u.pLayerInfo = saved_layer_info;
            chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
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

        std::lock_guard<std::mutex> lk(g_dev_lock);
        g_dev_map[*pDevice] = d;
        return VK_SUCCESS;
    }

    VK_LAYER_EXPORT void VKAPI_CALL
    FLM_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        {
            std::lock_guard<std::mutex> qlk(g_queue_lock);
            for (auto it = g_queue_map.begin(); it != g_queue_map.end(); ) {
                if (it->second.first == device) it = g_queue_map.erase(it);
                else ++it;
            }
        }

        std::lock_guard<std::mutex> lk(g_dev_lock);
        auto it = g_dev_map.find(device);
        if (it != g_dev_map.end()) {
            it->second.DestroyDevice(device, pAllocator);
            g_dev_map.erase(it);
        }
    }

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
                bool skip = st->hitch_active.load() || st->hitch_recovery_frames.load() > 0;

                if (!skip) {
                    int64_t target_ns = st->next_target_ns.load();
                    if (target_ns > 0) {
                        TimePoint target{NS(target_ns)};
                        auto now = Clock::now();
                        auto left = std::chrono::duration_cast<NS>(target - now);
                        if (left.count() > 0 && left.count() < 20000000LL) {
                            precise_wait_until(target);
                        } else if (left.count() >= 20000000LL) {
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

    VK_LAYER_EXPORT VkResult VKAPI_CALL
    FLM_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        DeviceDispatch* disp = nullptr;
        VkDevice        dev  = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> qlk(g_queue_lock);
            auto qit = g_queue_map.find(queue);
            if (qit != g_queue_map.end()) {
                dev  = qit->second.first;
                disp = qit->second.second;
            }
        }

        if (!disp) {
            std::lock_guard<std::mutex> lk(g_dev_lock);
            void* key = dispatch_key((void*)queue);
            for (auto& [d, dd] : g_dev_map) {
                if (dispatch_key((void*)d) == key) {
                    disp = &dd;
                    dev  = d;
                    break;
                }
            }
            if (disp) {
                std::lock_guard<std::mutex> qlk(g_queue_lock);
                g_queue_map[queue] = {dev, disp};
            }
        }
        if (!disp) return VK_ERROR_DEVICE_LOST;

        std::vector<uint64_t> present_ids(pPresentInfo->swapchainCount, 0);
        bool any_id = false;

        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            VkSwapchainKHR sc = pPresentInfo->pSwapchains[i];
            SwapchainState* st = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_sc_lock);
                auto it = g_sc_map.find(sc);
                if (it == g_sc_map.end()) {
                    auto* nst = new SwapchainState();
                    nst->device    = dev;
                    nst->swapchain = sc;
                    nst->disp      = disp;
                    g_sc_map[sc]   = nst;
                    st = nst;

                    if (disp->has_present_wait) {
                        nst->measure_running = true;
                        nst->measure_thread  = std::thread(measurement_thread_fn, nst);

                        int core_target = std::max(0, (int)std::thread::hardware_concurrency() - 2);
                        cpu_set_t cpuset;
                        CPU_ZERO(&cpuset);
                        CPU_SET(core_target, &cpuset);
                        pthread_setaffinity_np(nst->measure_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

                        if (g_rt_priority > 0) {
                            sched_param sp{};
                            sp.sched_priority = g_rt_priority;
                            pthread_setschedparam(nst->measure_thread.native_handle(), SCHED_FIFO, &sp);
                        }
                    }
                } else {
                    st = it->second;
                }
            }

            if (st && !st->hitch_active.load() && st->frame_count >= SwapchainState::WARMUP) {
                int64_t p_target = st->next_present_target_ns.load();
                if (p_target > 0) {
                    TimePoint target{NS(p_target)};
                    auto now = Clock::now();
                    auto left = std::chrono::duration_cast<NS>(target - now);
                    if (left.count() > 0 && left.count() < 20000000LL) {
                        precise_wait_until(target);
                    }
                }
            }

            if (disp->has_present_wait && st) {
                present_ids[i] = st->next_present_id.fetch_add(1);
                any_id = true;
            }
        }

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
