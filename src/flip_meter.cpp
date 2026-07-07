// ============================================================================
// FLM — Vulkan Flip Meter / Frame Pacing Layer  (v2 — "gerçek etki")
//
// TASARIM ÖZETİ
// -------------
// İki bağımsız yol:
//
//   1) LIMITER  — presentWait GEREKTİRMEZ. QueuePresent'te salt yerel saatle
//      absolute-timeline FPS sınırlayıcı (libstrangle mantığı). Her zaman
//      çalışır, MangoHud grafiğinde anında düz çizgi olarak görünür. Gözle
//      görülür, güvenli, deterministik etki. FLM_TARGET_FPS ister.
//
//   2) PACER    — presentWait VARSA. Ölçüm thread'i gerçek flip zamanlarını
//      okur, timeline tahmini kurar; MFG (frame-gen) çarpanı varsa üretilmiş
//      kareleri flip aralığına EŞİT dağıtır (slot pacing). VRR panelde düzgün
//      frametime için.
//
// ANTI-STUTTER KURALLARI (v1 placebo/stutter sebeplerinin çözümü):
//   * TEK KAPI: pacing yalnız TEK noktada (varsayılan Present). v1 hem Acquire
//     hem Present'te bekletiyordu → çift gecikme.
//   * GPU-BOUND BEKÇİSİ: oyun GPU-limitliyse CPU'da bekletmek kuyruğu boşaltıp
//     kötüleştirir. Ardışık kareler hedefi aşınca pacing otomatik kapanır.
//   * FIFO/vsync ATLAMA: FIFO zaten vsync'e kilitli; üstüne pacing = compositor
//     ile kavga. Yalnız MAILBOX/IMMEDIATE (VRR) pace edilir. Küçük yardımcı
//     swapchain'ler hiç pace edilmez.
//   * SOFT SLEW: timeline sapması hard-rebase yerine yumuşak düzeltilir.
//   * LEAD-BASED PRESENT: present tahmini flip'ten FLM_PRESENT_LEAD_NS önce
//     bırakılır (v1'in "+1 kare" hatalı formülü kaldırıldı).
//
// Kalıcı düzeltmeler (v1'den):
//   [FIX-1]  Hot-path shared_ptr kopyası (UAF).
//   [FIX-2]  DestroyDevice tüm state'leri stop+join.
//   [FIX-13] CreateDevice fallback'inde loader zinciri restore (MangoHud crash).
//   [FIX-14] pNext'te olan feature'lar tekrar enjekte edilmez.
//   [FIX-15] Oyunun kendi presentId'leri takip edilir (DXVK uyumu).
// ============================================================================

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#ifndef VK_LAYER_EXPORT
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define FLM_CPU_PAUSE() _mm_pause()
#else
#  define FLM_CPU_PAUSE() std::this_thread::yield()
#endif

using NS = std::chrono::nanoseconds;

// ============================================================================
// LOGGING
// ============================================================================
enum class LogLevel { DEBUG = 0, INFO, WARN, ERR };
static std::atomic<int> g_log_level{(int)LogLevel::ERR};   // [item 15] atomik
static FILE*            g_log_file = stderr;

#define FLM_LOG(level, ...) do { \
    if ((int)(level) >= g_log_level.load(std::memory_order_relaxed)) { \
        fprintf(g_log_file, "[FLM] " __VA_ARGS__); \
        fputc('\n', g_log_file); \
        fflush(g_log_file); \
    } \
} while (0)

// ============================================================================
// CONSTANTS
// ============================================================================
namespace FlmConst {
    constexpr int64_t  DEFAULT_INTERVAL_NS = 16'666'666LL;
    constexpr int64_t  DEFAULT_SPIN_NS     = 150'000LL;
    constexpr int64_t  DEFAULT_LEAD_NS     = 1'000'000LL;
    constexpr int      HITCH_RECOVERY      = 8;
    constexpr int      WARMUP_FRAMES       = 30;
    constexpr int      EMA_WARMUP          = 30;
    constexpr int      HISTORY_SIZE        = 8;
    constexpr uint64_t WAIT_TIMEOUT_NS     = 50'000'000ULL;
    constexpr int64_t  MAX_PACE_WAIT_NS    = 20'000'000LL;
    constexpr uint32_t STACK_PRESENT_IDS   = 8;
    constexpr int      GPU_BOUND_WINDOW    = 16;   // [item 8]
    constexpr int      MFG_DETECT_WINDOW   = 64;   // [item 7]
    constexpr int      MIN_SC_WIDTH        = 640;  // [item 11]
    constexpr int      MIN_SC_HEIGHT       = 480;
    constexpr int      CSV_BUFFER          = 256;  // [item 12]
}

enum class PaceMode  { AUTO = 0, PRESENT, LIMITER, OFF };
enum class PacePoint { PRESENT = 0, ACQUIRE, BOTH };

// ============================================================================
// CONFIG (hot-reload edilebilenler atomik — [item 15])
// ============================================================================
struct FLMConfig {
    // Yapısal (reload dışı)
    int         mfg_mult_env = 0;   // 0 = otomatik; >0 = zorla
    int         rt_priority  = 0;
    std::string measure_cpu;        // [item 13]
    bool        stats        = false;
    std::string csv_path;

    // Hot-reload edilebilir
    std::atomic<int>     target_fps {0};
    std::atomic<int64_t> spin_ns    {FlmConst::DEFAULT_SPIN_NS};
    std::atomic<int64_t> lead_ns    {FlmConst::DEFAULT_LEAD_NS};
    std::atomic<int64_t> drift_tol  {0};
    std::atomic<int>     mode       {(int)PaceMode::AUTO};
    std::atomic<int>     pace_point {(int)PacePoint::PRESENT};
};

static FLMConfig      g_config;
static std::once_flag g_config_flag;

static PaceMode parse_mode(const char* s) {
    if (!s) return PaceMode::AUTO;
    if (!strcmp(s, "present")) return PaceMode::PRESENT;
    if (!strcmp(s, "limiter")) return PaceMode::LIMITER;
    if (!strcmp(s, "off"))     return PaceMode::OFF;
    return PaceMode::AUTO;
}
static PacePoint parse_pace_point(const char* s) {
    if (!s) return PacePoint::PRESENT;
    if (!strcmp(s, "acquire")) return PacePoint::ACQUIRE;
    if (!strcmp(s, "both"))    return PacePoint::BOTH;
    return PacePoint::PRESENT;
}

// [item 15] SIGUSR1'de yalnız güvenli alanları yeniden oku.
static void reload_dynamic_config() {
    const char* e;
    if ((e = getenv("FLM_TARGET_FPS")))         g_config.target_fps.store(std::max(0, atoi(e)));
    if ((e = getenv("FLM_SPIN_NS")))            g_config.spin_ns.store(std::clamp<int64_t>(atoll(e), 0, 2'000'000LL));
    if ((e = getenv("FLM_PRESENT_LEAD_NS")))    g_config.lead_ns.store(std::clamp<int64_t>(atoll(e), 0, 8'000'000LL));
    if ((e = getenv("FLM_DRIFT_TOLERANCE_NS"))) g_config.drift_tol.store(std::max<int64_t>(0, atoll(e)));
    if ((e = getenv("FLM_MODE")))               g_config.mode.store((int)parse_mode(e));
    if ((e = getenv("FLM_PACE_POINT")))         g_config.pace_point.store((int)parse_pace_point(e));
    if ((e = getenv("FLM_LOG_LEVEL"))) {
        std::string l = e;
        if      (l == "DEBUG") g_log_level.store((int)LogLevel::DEBUG);
        else if (l == "INFO")  g_log_level.store((int)LogLevel::INFO);
        else if (l == "WARN")  g_log_level.store((int)LogLevel::WARN);
        else if (l == "ERROR") g_log_level.store((int)LogLevel::ERR);
    }
}

static void sigusr1_handler(int) { reload_dynamic_config(); }

static void init_config() {
    std::call_once(g_config_flag, []() {
        const char* e;
        if ((e = getenv("FLM_MFG_MULTIPLIER"))) g_config.mfg_mult_env = std::clamp(atoi(e), 0, 4);
        if ((e = getenv("FLM_RT_PRIORITY")))    g_config.rt_priority  = std::clamp(atoi(e), 0, 99);
        if ((e = getenv("FLM_MEASURE_CPU")))    g_config.measure_cpu  = e;
        if ((e = getenv("FLM_STATS")))          g_config.stats        = (atoi(e) != 0);
        if ((e = getenv("FLM_CSV")))            g_config.csv_path     = e;
        if ((e = getenv("FLM_LOG_FILE"))) { if (FILE* f = fopen(e, "a")) g_log_file = f; }

        reload_dynamic_config();

        // [item 15] SIGUSR1 yalnız kimse kullanmıyorsa kur.
        struct sigaction old{};
        if (sigaction(SIGUSR1, nullptr, &old) == 0 && old.sa_handler == SIG_DFL) {
            struct sigaction sa{};
            sa.sa_handler = sigusr1_handler;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGUSR1, &sa, nullptr);
        }

        FLM_LOG(LogLevel::INFO,
                "Config: mode=%d fps=%d mfg_env=%d spin=%lldns lead=%lldns rt=%d",
                g_config.mode.load(), g_config.target_fps.load(), g_config.mfg_mult_env,
                (long long)g_config.spin_ns.load(), (long long)g_config.lead_ns.load(),
                g_config.rt_priority);
    });
}

// ============================================================================
// TIMING
// ============================================================================
static inline int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

// Kaba kısım ABSTIME kernel uykusu (sinyal bölünmesine dayanıklı), son
// spin_ns kadar pause-spin. spin_ns=0 → tamamen uyku (min CPU).
static void precise_wait_absolute(int64_t target) {
    if (target <= 0) return;
    const int64_t spin = g_config.spin_ns.load(std::memory_order_relaxed);
    for (;;) {
        int64_t left = target - now_ns();
        if (left <= spin) break;
        int64_t wake = target - spin;
        timespec ts;
        ts.tv_sec  = wake / 1'000'000'000LL;
        ts.tv_nsec = wake % 1'000'000'000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    }
    while (now_ns() < target) FLM_CPU_PAUSE();
}

// ============================================================================
// DISPATCH
// ============================================================================
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr                     GetInstanceProcAddr      = nullptr;
    PFN_vkDestroyInstance                         DestroyInstance          = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2              GetPhysicalDeviceFeatures2 = nullptr; // [item 2]
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr           GetDeviceProcAddr           = nullptr;
    PFN_vkDestroyDevice               DestroyDevice               = nullptr;
    PFN_vkQueuePresentKHR             QueuePresentKHR             = nullptr;
    PFN_vkAcquireNextImageKHR         AcquireNextImageKHR         = nullptr;
    PFN_vkWaitForPresentKHR           WaitForPresentKHR           = nullptr;
    PFN_vkCreateSwapchainKHR          CreateSwapchainKHR          = nullptr;
    PFN_vkDestroySwapchainKHR         DestroySwapchainKHR         = nullptr;
    PFN_vkGetDeviceQueue              GetDeviceQueue              = nullptr;
    PFN_vkGetDeviceQueue2             GetDeviceQueue2             = nullptr;
    bool                              has_present_wait            = false;
};

// ============================================================================
// SWAPCHAIN STATE
// ============================================================================
struct SwapchainState {
    VkDevice        device    = VK_NULL_HANDLE;
    VkSwapchainKHR  swapchain = VK_NULL_HANDLE;
    DeviceDispatch* disp      = nullptr;

    // [item 11] Bağlam
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t         width  = 0;
    uint32_t         height = 0;
    bool             pace_allowed = false;   // create anında karar (FIFO/küçük → false)

    std::jthread    measure_thread;

    // Hot-path atomikleri — ayrı cache-line (false sharing engeli)
    alignas(64) std::atomic<uint64_t> next_present_id{1};
    alignas(64) std::atomic<int64_t>  gate_target_ns{0};        // TEK kapı hedefi
    alignas(64) std::atomic<int64_t>  base_flip_ns{0};          // slot 0 hedefi [item 4]
    alignas(64) std::atomic<int64_t>  slot_interval_ns{FlmConst::DEFAULT_INTERVAL_NS};
    alignas(64) std::atomic<uint32_t> present_seq{0};           // [item 4]
    alignas(64) std::atomic<int>      eff_mfg{1};               // [item 7] efektif çarpan
    alignas(64) std::atomic<int>      frame_count{0};
    alignas(64) std::atomic<bool>     hitch_active{false};
    alignas(64) std::atomic<int>      hitch_recovery_frames{0};
    alignas(64) std::atomic<bool>     app_owns_present_id{false};
    alignas(64) std::atomic<bool>     pacing_enabled{true};     // [item 8] GPU-bound bekçisi

    // LIMITER modu — QueuePresent thread'inde tutulan yerel timeline
    int64_t limiter_next_ns = 0;

    // Yalnız ölçüm thread'i dokunur → kilitsiz
    NS      display_intervals[FlmConst::HISTORY_SIZE] = {};
    int     di_idx      = 0;
    int     di_count    = 0;
    int     ema_frames  = 0;
    int64_t filtered_interval_ns = FlmConst::DEFAULT_INTERVAL_NS;
    int64_t timeline_target_ns   = 0;
    // [item 8] GPU-bound penceresi
    int     over_target_run  = 0;
    int     under_target_run = 0;
    // [item 7] MFG algılama
    int     mfg_small_cnt = 0;
    int     mfg_total_cnt = 0;
    // [item 12] istatistik
    int64_t stat_last_ns   = 0;
    int64_t stat_sum_ns    = 0;
    int64_t stat_max_ns    = 0;
    int     stat_frames    = 0;
    int     stat_fake      = 0;
    // [item 12] CSV
    FILE*   csv_fp = nullptr;
    struct CsvRow { int64_t flip_ns, interval_ns; int is_fake, is_hitch; uint32_t slot; };
    CsvRow  csv_buf[FlmConst::CSV_BUFFER];
    int     csv_n = 0;

    SwapchainState(VkDevice dev, VkSwapchainKHR sc, DeviceDispatch* d)
        : device(dev), swapchain(sc), disp(d) {}

    ~SwapchainState() {
        if (csv_n && csv_fp) csv_flush();
        if (csv_fp) fclose(csv_fp);
    }

    int64_t get_hitch_threshold(int64_t avg_ns) const {
        int64_t adaptive = std::max<int64_t>((avg_ns * 3) / 2, avg_ns + 2'000'000LL);
        return std::min<int64_t>(adaptive, avg_ns + 30'000'000LL);
    }

    NS get_median_interval() const {
        int n = std::min(di_count, FlmConst::HISTORY_SIZE);
        if (n == 0) return NS(filtered_interval_ns);
        NS tmp[FlmConst::HISTORY_SIZE];
        std::copy(display_intervals, display_intervals + n, tmp);
        std::sort(tmp, tmp + n);
        return tmp[n / 2];
    }

    void csv_flush() {
        if (!csv_fp) return;
        for (int i = 0; i < csv_n; i++) {
            fprintf(csv_fp, "%lld,%lld,%d,%d,%u\n",
                    (long long)csv_buf[i].flip_ns, (long long)csv_buf[i].interval_ns,
                    csv_buf[i].is_fake, csv_buf[i].is_hitch, csv_buf[i].slot);
        }
        fflush(csv_fp);
        csv_n = 0;
    }
    void csv_push(int64_t flip, int64_t interval, bool fake, bool hitch, uint32_t slot) {
        if (!csv_fp) return;
        csv_buf[csv_n++] = {flip, interval, fake ? 1 : 0, hitch ? 1 : 0, slot};
        if (csv_n >= FlmConst::CSV_BUFFER) csv_flush();
    }
};

// ============================================================================
// GLOBAL MAPS
// ============================================================================
static std::shared_mutex g_inst_lock;
static std::unordered_map<VkInstance, InstanceDispatch> g_inst_map;
// [item 2] dispatch_key(gpu/instance) → InstanceDispatch bulmak için
static std::unordered_map<void*, VkInstance>            g_instkey_map;

static std::shared_mutex g_dev_lock;
static std::unordered_map<VkDevice, DeviceDispatch> g_dev_map;

struct QueueData {
    VkDevice        device = VK_NULL_HANDLE;
    DeviceDispatch* disp   = nullptr;
};
static std::shared_mutex g_queue_lock;
static std::unordered_map<VkQueue, QueueData> g_queue_map;

static std::shared_mutex g_sc_lock;
static std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainState>> g_sc_map;

static inline void* dispatch_key(void* handle) { return *(void**)handle; }

static DeviceDispatch* find_device_dispatch(VkDevice device) {
    std::shared_lock lk(g_dev_lock);
    auto it = g_dev_map.find(device);
    return (it != g_dev_map.end()) ? &it->second : nullptr;
}

static std::shared_ptr<SwapchainState> find_sc_state(VkSwapchainKHR sc) {
    std::shared_lock lk(g_sc_lock);
    auto it = g_sc_map.find(sc);
    return (it != g_sc_map.end()) ? it->second : nullptr;  // [FIX-1] kopya
}

static void stop_and_join(std::shared_ptr<SwapchainState>& st) {
    if (st && st->measure_thread.joinable()) {
        st->measure_thread.request_stop();
        st->measure_thread.join();
    }
}

// ============================================================================
// MEASUREMENT THREAD
// ----------------------------------------------------------------------------
// GÖREV: gerçek flip aralığını ölç, doğal cadence'ı (fake-filtreli) yayınla,
// GPU-bound / hitch durumunu işaretle. GATING BURADA YAPILMAZ — kapı present
// thread'inde yerel timeline ile çalışır (cross-thread mutlak hedef devri
// v1'deki stutter'ın kaynağıydı; kaldırıldı).
// ============================================================================
static void apply_thread_policies() {
    if (g_config.rt_priority > 0) {
        sched_param sp{};
        sp.sched_priority = g_config.rt_priority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            FLM_LOG(LogLevel::WARN, "SCHED_FIFO ayarlanamadi (CAP_SYS_NICE?)");
    }
    // [item 13] ölçüm thread affinity: "0-3" veya "5"
    if (!g_config.measure_cpu.empty()) {
        cpu_set_t set; CPU_ZERO(&set);
        const std::string& s = g_config.measure_cpu;
        size_t dash = s.find('-');
        bool ok = false;
        try {
            if (dash != std::string::npos) {
                int a = std::stoi(s.substr(0, dash));
                int b = std::stoi(s.substr(dash + 1));
                if (a >= 0 && b >= a && b < CPU_SETSIZE) {
                    for (int c = a; c <= b; c++) CPU_SET(c, &set);
                    ok = true;
                }
            } else {
                int c = std::stoi(s);
                if (c >= 0 && c < CPU_SETSIZE) { CPU_SET(c, &set); ok = true; }
            }
        } catch (...) { ok = false; }
        if (ok) {
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
                FLM_LOG(LogLevel::WARN, "FLM_MEASURE_CPU affinity ayarlanamadi");
        } else {
            FLM_LOG(LogLevel::WARN, "FLM_MEASURE_CPU parse hatasi: %s", s.c_str());
        }
    }
    pthread_setname_np(pthread_self(), "flm-measure");
}

static void measurement_thread_fn(std::stop_token stoken, std::shared_ptr<SwapchainState> st) {
    apply_thread_policies();

    // [item 12] CSV aç
    if (!g_config.csv_path.empty()) {
        st->csv_fp = fopen(g_config.csv_path.c_str(), "w");
        if (st->csv_fp) fprintf(st->csv_fp, "flip_ns,interval_ns,is_fake,is_hitch,slot\n");
    }

    uint64_t wait_id         = st->next_present_id.load(std::memory_order_relaxed);
    if (wait_id == 0) wait_id = 1;
    int64_t  last_display_ns = 0;
    bool     last_valid      = false;
    st->stat_last_ns = now_ns();

    while (!stoken.stop_requested()) {
        if (!st->disp || !st->disp->has_present_wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // [FIX-5] Geride kaldıysak ileri sar (eski id anında döner → ~0 interval)
        uint64_t latest = st->next_present_id.load(std::memory_order_relaxed);
        if (latest > 2 && wait_id + 2 < latest) {
            wait_id    = latest - 1;
            last_valid = false;
        }

        VkResult r = st->disp->WaitForPresentKHR(st->device, st->swapchain,
                                                 wait_id, FlmConst::WAIT_TIMEOUT_NS);
        if (r == VK_TIMEOUT) { last_valid = false; continue; }
        // [item 9] resize/alt-tab: swapchain yaşıyor, thread ÖLMEMELİ.
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR ||
            r == VK_ERROR_SURFACE_LOST_KHR) {
            last_valid = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (r != VK_SUCCESS) {
            FLM_LOG(LogLevel::DEBUG, "WaitForPresentKHR fatal: %d", (int)r);
            break;  // yalnız DEVICE_LOST / bilinmeyen
        }

        int64_t tnow = now_ns();

        if (last_valid) {
            int64_t interval_ns   = tnow - last_display_ns;
            int64_t avg_before_ns = st->get_median_interval().count();

            // Efektif çarpan
            int m = st->eff_mfg.load(std::memory_order_relaxed);

            // Fake-frame sınıflandırma (yalnız FİLTRE için — pacing artık her
            // present'i uniform aralıklar; fake tespiti EMA'yı korur).
            bool is_fake = false;
            if (m > 1 && st->di_count > 4) {
                int64_t split_ns = (avg_before_ns * (m + 1)) / (2LL * m);
                is_fake = (interval_ns < split_ns) ||
                          (interval_ns > (avg_before_ns * 5) / 2);
            }

            // [item 7] MFG algılama: split-altı oranı
            {
                int64_t split_ns = (avg_before_ns * 3) / 4; // ~m=2 eşiği referans
                if (interval_ns < split_ns) st->mfg_small_cnt++;
                st->mfg_total_cnt++;
                if (st->mfg_total_cnt >= FlmConst::MFG_DETECT_WINDOW) {
                    if (g_config.mfg_mult_env > 0) {
                        st->eff_mfg.store(g_config.mfg_mult_env, std::memory_order_relaxed);
                    } else {
                        double p = (double)st->mfg_small_cnt / (double)st->mfg_total_cnt;
                        int mhat = (p < 0.99) ? (int)std::lround(1.0 / (1.0 - p)) : 4;
                        mhat = std::clamp(mhat, 1, 4);
                        st->eff_mfg.store(mhat, std::memory_order_relaxed);
                    }
                    st->mfg_small_cnt = 0;
                    st->mfg_total_cnt = 0;
                }
            }

            bool is_hitch = false;
            if (is_fake) {
                // Fake kareyi filtre dışı bırak; cadence bozulmasın.
            } else {
                is_hitch = interval_ns > st->get_hitch_threshold(avg_before_ns);
                if (is_hitch) {
                    st->hitch_active.store(true, std::memory_order_relaxed);
                    st->hitch_recovery_frames.store(FlmConst::HITCH_RECOVERY,
                                                    std::memory_order_relaxed);
                } else if (st->hitch_active.load(std::memory_order_relaxed)) {
                    if (st->hitch_recovery_frames.fetch_sub(1, std::memory_order_relaxed) <= 1)
                        st->hitch_active.store(false, std::memory_order_relaxed);
                }

                st->display_intervals[st->di_idx] = NS(interval_ns);
                st->di_idx = (st->di_idx + 1) % FlmConst::HISTORY_SIZE;
                if (st->di_count < FlmConst::HISTORY_SIZE) st->di_count++;
                if (st->ema_frames < FlmConst::EMA_WARMUP) st->ema_frames++;

                int64_t safe = is_hitch ? avg_before_ns : interval_ns;
                // [item 16] yarım-yukarı yuvarlamalı EMA
                if (st->ema_frames < FlmConst::EMA_WARMUP)
                    st->filtered_interval_ns += (safe - st->filtered_interval_ns + 2) * 4 / 10;
                else
                    st->filtered_interval_ns += (safe - st->filtered_interval_ns + 5) / 10;
            }

            // Yayınlanacak slot aralığı: hedef fps varsa ondan, yoksa doğal cadence.
            int fps = g_config.target_fps.load(std::memory_order_relaxed);
            int64_t slot_iv = (fps > 0) ? (1'000'000'000LL / fps)
                                        : st->filtered_interval_ns;
            st->slot_interval_ns.store(slot_iv, std::memory_order_relaxed);

            // [item 8] GPU-bound bekçisi: hedefi tutturamıyorsak pacing'i kapat.
            if (!is_fake && !is_hitch) {
                int64_t tgt = slot_iv;
                if (interval_ns > (tgt * 105) / 100) {
                    st->over_target_run++; st->under_target_run = 0;
                } else if (interval_ns <= (tgt * 102) / 100) {
                    st->under_target_run++; st->over_target_run = 0;
                }
                if (st->over_target_run >= FlmConst::GPU_BOUND_WINDOW) {
                    if (st->pacing_enabled.exchange(false, std::memory_order_relaxed))
                        FLM_LOG(LogLevel::DEBUG, "GPU-bound: pacing OFF");
                    st->over_target_run = FlmConst::GPU_BOUND_WINDOW;
                } else if (st->under_target_run >= FlmConst::GPU_BOUND_WINDOW) {
                    if (!st->pacing_enabled.exchange(true, std::memory_order_relaxed))
                        FLM_LOG(LogLevel::DEBUG, "GPU-bound: pacing ON");
                    st->under_target_run = FlmConst::GPU_BOUND_WINDOW;
                }
            }

            // [item 12] istatistik + CSV
            if (!is_fake) {
                st->stat_sum_ns   += interval_ns;
                st->stat_max_ns    = std::max(st->stat_max_ns, interval_ns);
                st->stat_frames++;
                if (is_hitch) st->stat_fake++; // hitch sayacı (isim gevşek)
            } else {
                st->stat_fake++;
            }
            st->csv_push(tnow, interval_ns, is_fake, is_hitch,
                         st->present_seq.load(std::memory_order_relaxed));

            if (g_config.stats && tnow - st->stat_last_ns >= 5'000'000'000LL &&
                st->stat_frames > 0) {
                double avg_ms = (st->stat_sum_ns / (double)st->stat_frames) / 1e6;
                double max_ms = st->stat_max_ns / 1e6;
                FLM_LOG(LogLevel::INFO,
                    "STATS %ds: n=%d avg=%.2fms max=%.2fms fake/hitch=%d mfg=%d pacing=%d",
                    5, st->stat_frames, avg_ms, max_ms, st->stat_fake,
                    st->eff_mfg.load(), (int)st->pacing_enabled.load());
                st->stat_sum_ns = st->stat_max_ns = 0;
                st->stat_frames = st->stat_fake = 0;
                st->stat_last_ns = tnow;
            }
        }

        last_display_ns = tnow;
        last_valid      = true;
        wait_id++;
    }

    if (st->csv_n) st->csv_flush();
    FLM_LOG(LogLevel::DEBUG, "Olcum thread'i durdu");
}

// ============================================================================
// LAYER HOOKS
// ============================================================================
extern "C" {

// Forward
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL FLM_vkGetInstanceProcAddr(VkInstance, const char*);
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL FLM_vkGetDeviceProcAddr(VkDevice, const char*);

VK_LAYER_EXPORT VkResult VKAPI_CALL FLM_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    init_config();

    auto* chain = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    auto fn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = fn(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceDispatch d{};
    d.GetInstanceProcAddr        = (PFN_vkGetInstanceProcAddr)gipa(*pInstance, "vkGetInstanceProcAddr");
    d.DestroyInstance            = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
    // [item 2] core 1.1 fonksiyonu; yoksa KHR türevini dene.
    d.GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*pInstance, "vkGetPhysicalDeviceFeatures2");
    if (!d.GetPhysicalDeviceFeatures2)
        d.GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*pInstance, "vkGetPhysicalDeviceFeatures2KHR");

    std::unique_lock lk(g_inst_lock);
    g_inst_map[*pInstance]                       = d;
    g_instkey_map[dispatch_key((void*)*pInstance)] = *pInstance;  // [item 2]
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL FLM_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    InstanceDispatch d{};
    {
        std::unique_lock lk(g_inst_lock);
        auto it = g_inst_map.find(instance);
        if (it != g_inst_map.end()) { d = it->second; g_inst_map.erase(it); }
        g_instkey_map.erase(dispatch_key((void*)instance));
    }
    if (d.DestroyInstance) d.DestroyInstance(instance, pAllocator);
}

// [item 2] presentId + presentWait feature'larını gerçekten destekliyor mu?
static bool query_present_features(VkPhysicalDevice gpu) {
    InstanceDispatch inst{};
    {
        std::shared_lock lk(g_inst_lock);
        auto kit = g_instkey_map.find(dispatch_key((void*)gpu));
        if (kit != g_instkey_map.end()) {
            auto it = g_inst_map.find(kit->second);
            if (it != g_inst_map.end()) inst = it->second;
        }
    }
    if (!inst.GetPhysicalDeviceFeatures2) return false; // sorgulayamıyoruz → güvenli taraf

    VkPhysicalDevicePresentIdFeaturesKHR   id_f{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr, VK_FALSE};
    VkPhysicalDevicePresentWaitFeaturesKHR wait_f{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, &id_f, VK_FALSE};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &wait_f, {}};
    inst.GetPhysicalDeviceFeatures2(gpu, &f2);
    return id_f.presentId && wait_f.presentWait;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL FLM_vkCreateDevice(
    VkPhysicalDevice gpu, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    auto* chain = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    // [FIX-13] Retry için zincir konumunu SAKLA (loader'ın paylaşılan mutable
    // struct'ı; alt katmanlar da ilerletir, restore etmeden 2. çağrı = crash).
    VkLayerDeviceLink* next_link = chain->u.pLayerInfo;

    // Oyunun uzantı listesi
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames +
                                  pCreateInfo->enabledExtensionCount);
    bool app_has_id = false, app_has_wait = false;
    for (auto& e : exts) {
        if (!strcmp(e, VK_KHR_PRESENT_ID_EXTENSION_NAME))   app_has_id   = true;
        if (!strcmp(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) app_has_wait = true;
    }

    // [item 2] presentWait yalnız sürücü gerçekten destekliyorsa enjekte et.
    bool want_inject = query_present_features(gpu);
    if (!want_inject)
        FLM_LOG(LogLevel::INFO, "presentId/Wait desteklenmiyor; PACER kapali (LIMITER hala kullanilabilir)");

    bool injected = false;
    if (want_inject) {
        if (!app_has_id)   exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        if (!app_has_wait) exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        injected = true;
    }

    // [FIX-14] pNext'te zaten olan feature struct'larını tekrar ekleme.
    bool chain_id_feat = false, chain_wait_feat = false;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)pCreateInfo->pNext;
         p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)   chain_id_feat   = true;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR) chain_wait_feat = true;
    }

    VkPhysicalDevicePresentIdFeaturesKHR   id_feat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr, VK_TRUE};
    VkPhysicalDevicePresentWaitFeaturesKHR wait_feat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, nullptr, VK_TRUE};

    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledExtensionCount   = (uint32_t)exts.size();
    const void* tail = ci.pNext;
    if (want_inject && !chain_wait_feat) { wait_feat.pNext = (void*)tail; tail = &wait_feat; }
    if (want_inject && !chain_id_feat)   { id_feat.pNext   = (void*)tail; tail = &id_feat; }
    ci.pNext = tail;

    auto fn = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = fn(gpu, &ci, pAllocator, pDevice);
    bool created_with_wait = injected && (res == VK_SUCCESS);
    if (res != VK_SUCCESS && injected) {
        FLM_LOG(LogLevel::WARN, "presentWait ile CreateDevice basarisiz (%d), fallback", (int)res);
        chain->u.pLayerInfo = next_link;               // [FIX-13] restore
        res = fn(gpu, pCreateInfo, pAllocator, pDevice);
        created_with_wait = false;
    }
    if (res != VK_SUCCESS) return res;

    DeviceDispatch d{};
    d.GetDeviceProcAddr   = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
    d.DestroyDevice       = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
    d.QueuePresentKHR     = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");
    d.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(*pDevice, "vkAcquireNextImageKHR");
    d.WaitForPresentKHR   = (PFN_vkWaitForPresentKHR)gdpa(*pDevice, "vkWaitForPresentKHR");
    d.CreateSwapchainKHR  = (PFN_vkCreateSwapchainKHR)gdpa(*pDevice, "vkCreateSwapchainKHR");
    d.DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(*pDevice, "vkDestroySwapchainKHR");
    d.GetDeviceQueue      = (PFN_vkGetDeviceQueue)gdpa(*pDevice, "vkGetDeviceQueue");
    d.GetDeviceQueue2     = (PFN_vkGetDeviceQueue2)gdpa(*pDevice, "vkGetDeviceQueue2");

    // [item 1] presentWait'i yalnız GÜVENLE enable edildiğinde kullan.
    // Fallback yolunda uzantı enable EDİLMEDİ; WaitForPresentKHR non-null
    // dönebilir ama çağırmak UB. created_with_wait bunu garanti eder; ayrıca
    // oyunun kendisi ikisini de enable etmişse yine güvenli.
    bool safe_wait = created_with_wait || (app_has_id && app_has_wait);
    d.has_present_wait = (d.WaitForPresentKHR != nullptr) && safe_wait;

    std::unique_lock lk(g_dev_lock);
    g_dev_map[*pDevice] = d;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL FLM_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    // [FIX-2] Bu device'a ait tüm swapchain state'lerini durdur+join.
    std::vector<std::shared_ptr<SwapchainState>> orphans;
    {
        std::unique_lock lk(g_sc_lock);
        for (auto it = g_sc_map.begin(); it != g_sc_map.end();) {
            if (it->second->device == device) {
                orphans.push_back(std::move(it->second));
                it = g_sc_map.erase(it);
            } else ++it;
        }
    }
    for (auto& st : orphans) stop_and_join(st);

    {
        std::unique_lock qlk(g_queue_lock);
        std::erase_if(g_queue_map, [device](const auto& kv) {
            return kv.second.device == device;
        });
    }

    DeviceDispatch d{};
    {
        std::unique_lock lk(g_dev_lock);
        auto it = g_dev_map.find(device);
        if (it != g_dev_map.end()) { d = it->second; g_dev_map.erase(it); }
    }
    if (d.DestroyDevice) d.DestroyDevice(device, pAllocator);
}

// [FIX-9] Queue map'i create anında doldur.
VK_LAYER_EXPORT void VKAPI_CALL FLM_vkGetDeviceQueue(
    VkDevice device, uint32_t qf, uint32_t qi, VkQueue* pQueue)
{
    DeviceDispatch* disp = find_device_dispatch(device);
    if (!disp || !disp->GetDeviceQueue) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    disp->GetDeviceQueue(device, qf, qi, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        std::unique_lock lk(g_queue_lock);
        g_queue_map[*pQueue] = QueueData{device, disp};
    }
}

VK_LAYER_EXPORT void VKAPI_CALL FLM_vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue)
{
    DeviceDispatch* disp = find_device_dispatch(device);
    if (!disp || !disp->GetDeviceQueue2) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    disp->GetDeviceQueue2(device, pInfo, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        std::unique_lock lk(g_queue_lock);
        g_queue_map[*pQueue] = QueueData{device, disp};
    }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL FLM_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    DeviceDispatch* disp = find_device_dispatch(device);
    if (!disp || !disp->CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = disp->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) return res;

    auto st = std::make_shared<SwapchainState>(device, *pSwapchain, disp);
    st->present_mode = pCreateInfo->presentMode;
    st->width        = pCreateInfo->imageExtent.width;
    st->height       = pCreateInfo->imageExtent.height;
    st->next_present_id.store(1, std::memory_order_relaxed);

    // [item 11] Pacing kararı:
    //  - Küçük yardımcı swapchain (launcher/overlay) → hiç pace etme.
    //  - FIFO/FIFO_RELAXED zaten vsync'e kilitli → present pacing gereksiz
    //    (yalnız ACQUIRE pace point'i seçilirse acquire kapısına izin var,
    //     bu QueuePresent tarafında kontrol ediliyor).
    //  - MAILBOX/IMMEDIATE (VRR senaryosu) → present pacing serbest.
    bool too_small = (st->width  < (uint32_t)FlmConst::MIN_SC_WIDTH ||
                      st->height < (uint32_t)FlmConst::MIN_SC_HEIGHT);
    st->pace_allowed = !too_small;

    if (too_small) {
        FLM_LOG(LogLevel::DEBUG, "Kucuk swapchain %ux%u — pacing atlandi",
                st->width, st->height);
    }

    // Ölçüm thread'i yalnız presentWait varsa ve pace edilebilir swapchain için.
    if (disp->has_present_wait && st->pace_allowed)
        st->measure_thread = std::jthread(measurement_thread_fn, st);

    std::shared_ptr<SwapchainState> stale;
    {
        std::unique_lock lk(g_sc_lock);
        auto it = g_sc_map.find(*pSwapchain);
        if (it != g_sc_map.end()) stale = std::move(it->second);
        g_sc_map[*pSwapchain] = std::move(st);
    }
    stop_and_join(stale);
    return res;
}

VK_LAYER_EXPORT void VKAPI_CALL FLM_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    DeviceDispatch* disp = find_device_dispatch(device);

    if (swapchain != VK_NULL_HANDLE) {
        std::shared_ptr<SwapchainState> st;
        {
            std::unique_lock lk(g_sc_lock);
            auto it = g_sc_map.find(swapchain);
            if (it != g_sc_map.end()) { st = std::move(it->second); g_sc_map.erase(it); }
        }
        stop_and_join(st);
    }
    if (disp && disp->DestroySwapchainKHR)
        disp->DestroySwapchainKHR(device, swapchain, pAllocator);
}

// ============================================================================
// GATE — present thread'inde çalışan TEK kapı (limiter + pacer birleşik)
// ----------------------------------------------------------------------------
// Yerel timeline (st->limiter_next_ns) kullanır. KRİTİK ÖZELLİK: kapı hedefi
// geçmişteyse HİÇ beklemez — yani pacing yalnızca GECİKTİREBİLİR, asla
// hızlandırmaz. Bu yüzden oyunu "kaçırmaya" zorlayamaz; en kötü ihtimalle
// hiçbir şey yapmaz. Stutter üretmemesinin temel garantisi budur.
// ============================================================================
static void apply_gate(SwapchainState* st, bool limiter_mode) {
    // Hitch veya toparlanma: pace etme, timeline'ı sıfırla (temiz yeniden anchor).
    if (st->hitch_active.load(std::memory_order_relaxed) ||
        st->hitch_recovery_frames.load(std::memory_order_relaxed) > 0) {
        st->limiter_next_ns = 0;
        return;
    }
    // [item 8] GPU-bound → pace etme.
    if (!st->pacing_enabled.load(std::memory_order_relaxed)) {
        st->limiter_next_ns = 0;
        return;
    }

    int64_t iv, lead;
    if (limiter_mode) {
        int fps = g_config.target_fps.load(std::memory_order_relaxed);
        if (fps <= 0) return;
        iv   = 1'000'000'000LL / fps;
        lead = 0;  // limiter tam hedefe kilitler
    } else {
        // [item 3] PACER: doğal cadence'a uniform aralık, flip'ten lead önce.
        iv = st->slot_interval_ns.load(std::memory_order_relaxed);
        if (iv <= 0) return;
        lead = g_config.lead_ns.load(std::memory_order_relaxed);
    }

    int64_t t = now_ns();
    if (st->limiter_next_ns == 0) {
        st->limiter_next_ns = t + iv;   // ilk kare: bekletme, timeline'ı kur
        return;
    }
    st->limiter_next_ns += iv;          // [item 4] uniform slot ilerlemesi

    // [item 10] Soft slew: hard-rebase yalnız aşırı sapmada; küçük borcu
    // ~8 karede yumuşakça kapat (görünür faz sıçraması yok).
    int64_t drift = st->limiter_next_ns - t;
    int64_t tol   = g_config.drift_tol.load(std::memory_order_relaxed);
    if (tol <= 0) tol = std::clamp<int64_t>(iv / 4, 1'000'000LL, 4'000'000LL);
    if (drift < -2 * iv || drift > 4 * iv) {
        st->limiter_next_ns = t + iv;   // stall / clock jump
    } else if (drift < -tol) {
        st->limiter_next_ns -= drift / 8;  // drift<0 → hedefi öne çek
    }

    int64_t target = st->limiter_next_ns - lead;
    int64_t left   = target - t;
    if (left > 0 && left < FlmConst::MAX_PACE_WAIT_NS)
        precise_wait_absolute(target);
}

// Etkin modu çöz. limiter_mode çıktısı: gate limiter mantığı mı kullanacak.
// return: pace edilecek mi.
static bool resolve_gate(const SwapchainState* st, bool has_wait, bool& limiter_mode) {
    if (!st->pace_allowed) return false;
    PaceMode mode = (PaceMode)g_config.mode.load(std::memory_order_relaxed);
    int      fps  = g_config.target_fps.load(std::memory_order_relaxed);

    // [item 11] FIFO/FIFO_RELAXED zaten vsync'e kilitli → PACER (uniform
    // cadence tahmini) gereksiz ve compositor ile kavga eder. LIMITER (daha
    // düşük fps'e cap) bu modlarda YİNE geçerli ve faydalı, ona dokunma.
    bool is_fifo = (st->present_mode == VK_PRESENT_MODE_FIFO_KHR ||
                    st->present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR);

    switch (mode) {
        case PaceMode::OFF:     return false;
        case PaceMode::LIMITER: limiter_mode = true;  return fps > 0;
        case PaceMode::PRESENT:
            if (has_wait && !is_fifo) { limiter_mode = false; return true; }
            limiter_mode = true; return fps > 0;   // FIFO veya wait yok → limiter
        case PaceMode::AUTO:
        default:
            if (has_wait && !is_fifo) { limiter_mode = false; return true; }
            limiter_mode = true; return fps > 0;
    }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL FLM_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
{
    DeviceDispatch* disp = find_device_dispatch(device);
    if (!disp) return VK_ERROR_DEVICE_LOST;

    // [item 6] Yalnız pace_point ACQUIRE/BOTH ise burada kapı uygula.
    PacePoint pp = (PacePoint)g_config.pace_point.load(std::memory_order_relaxed);
    if (pp == PacePoint::ACQUIRE || pp == PacePoint::BOTH) {
        if (auto st = find_sc_state(swapchain)) {   // [FIX-1] shared_ptr kopya
            if (st->frame_count.load(std::memory_order_relaxed) >= FlmConst::WARMUP_FRAMES) {
                bool limiter_mode = false;
                if (resolve_gate(st.get(), disp->has_present_wait, limiter_mode))
                    apply_gate(st.get(), limiter_mode);
            }
            st->frame_count.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // Pacing present'te; yine de frame_count ilerlesin (warmup için).
        if (auto st = find_sc_state(swapchain))
            st->frame_count.fetch_add(1, std::memory_order_relaxed);
    }

    return disp->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL FLM_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    QueueData qdata{};
    {
        std::shared_lock qlk(g_queue_lock);
        auto qit = g_queue_map.find(queue);
        if (qit != g_queue_map.end()) qdata = qit->second;
    }
    if (!qdata.disp) {   // yedek: dispatch-key ile çöz+cache
        {
            std::shared_lock lk(g_dev_lock);
            void* key = dispatch_key((void*)queue);
            for (auto& [d, dd] : g_dev_map)
                if (dispatch_key((void*)d) == key) { qdata.disp = &dd; qdata.device = d; break; }
        }
        if (qdata.disp) { std::unique_lock qlk(g_queue_lock); g_queue_map[queue] = qdata; }
    }
    if (!qdata.disp) return VK_ERROR_DEVICE_LOST;

    const uint32_t sc_count = pPresentInfo->swapchainCount;
    const bool has_wait = qdata.disp->has_present_wait;

    // [FIX-15] Oyunun kendi VkPresentIdKHR'ı var mı?
    const VkPresentIdKHR* app_pid = nullptr;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)pPresentInfo->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) { app_pid = (const VkPresentIdKHR*)p; break; }
    const bool app_has_present_id = (app_pid != nullptr);

    // [FIX-4] Stack dizileri (≤8 swapchain → heap yok)
    uint64_t ids_stack[FlmConst::STACK_PRESENT_IDS];
    std::vector<uint64_t> ids_heap;
    uint64_t* present_ids = ids_stack;
    if (sc_count > FlmConst::STACK_PRESENT_IDS) { ids_heap.resize(sc_count, 0); present_ids = ids_heap.data(); }
    else std::fill(ids_stack, ids_stack + sc_count, 0ULL);

    std::shared_ptr<SwapchainState> states_stack[FlmConst::STACK_PRESENT_IDS];
    std::vector<std::shared_ptr<SwapchainState>> states_heap;
    std::shared_ptr<SwapchainState>* states = states_stack;
    if (sc_count > FlmConst::STACK_PRESENT_IDS) { states_heap.resize(sc_count); states = states_heap.data(); }

    // [item 6] present kapısı yalnız pace_point PRESENT/BOTH ise.
    PacePoint pp = (PacePoint)g_config.pace_point.load(std::memory_order_relaxed);
    bool gate_here = (pp == PacePoint::PRESENT || pp == PacePoint::BOTH);

    bool any_id = false;
    for (uint32_t i = 0; i < sc_count; i++) {
        auto st = find_sc_state(pPresentInfo->pSwapchains[i]);
        states[i] = st;
        if (!st) continue;

        if (app_has_present_id) {
            st->app_owns_present_id.store(true, std::memory_order_relaxed);
            // [FIX-15] Oyunun id'sini takip et: next_present_id = app_id + 1.
            if (app_pid->pPresentIds && i < app_pid->swapchainCount) {
                uint64_t id = app_pid->pPresentIds[i];
                if (id) st->next_present_id.store(id + 1, std::memory_order_relaxed);
            }
        }

        // TEK KAPI (yalnız ilk/primary swapchain'de; çoklu swapchain nadir)
        if (gate_here && i == 0 &&
            st->frame_count.load(std::memory_order_relaxed) >= FlmConst::WARMUP_FRAMES) {
            st->present_seq.fetch_add(1, std::memory_order_relaxed);  // [item 4]
            bool limiter_mode = false;
            if (resolve_gate(st.get(), has_wait, limiter_mode))
                apply_gate(st.get(), limiter_mode);
        }

        // presentWait için id enjekte et (oyun kendisi göndermiyorsa).
        if (has_wait && !app_has_present_id) {
            present_ids[i] = st->next_present_id.fetch_add(1, std::memory_order_relaxed);
            any_id = true;
        }
    }

    VkPresentIdKHR   present_id_info{};
    VkPresentInfoKHR modified = *pPresentInfo;
    if (any_id && !app_has_present_id && has_wait) {
        present_id_info.sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        present_id_info.swapchainCount = sc_count;
        present_id_info.pPresentIds    = present_ids;
        present_id_info.pNext          = pPresentInfo->pNext;
        modified.pNext                 = &present_id_info;
    }

    return qdata.disp->QueuePresentKHR(queue, &modified);
}

// ============================================================================
// PROC ADDR
// ============================================================================
#define INTERCEPT(fn) if (strcmp(pName, "vk" #fn) == 0) return (PFN_vkVoidFunction)FLM_vk##fn

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL FLM_vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    INTERCEPT(GetDeviceProcAddr);
    INTERCEPT(DestroyDevice);
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(AcquireNextImageKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);

    std::shared_lock lk(g_dev_lock);
    auto it = g_dev_map.find(device);
    if (it == g_dev_map.end() || !it->second.GetDeviceProcAddr) return nullptr;
    return it->second.GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL FLM_vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    INTERCEPT(GetInstanceProcAddr);
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(GetDeviceProcAddr);
    // [FIX-11] GIPA üzerinden istenen device-level fonksiyonlar da katmandan geçmeli.
    INTERCEPT(DestroyDevice);
    INTERCEPT(QueuePresentKHR);
    INTERCEPT(AcquireNextImageKHR);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);

    if (instance == VK_NULL_HANDLE) return nullptr;
    std::shared_lock lk(g_inst_lock);
    auto it = g_inst_map.find(instance);
    if (it == g_inst_map.end() || !it->second.GetInstanceProcAddr) return nullptr;
    return it->second.GetInstanceProcAddr(instance, pName);
}

#undef INTERCEPT

// ============================================================================
// [item 14] LOADER INTERFACE v2 NEGOTIATION
// ============================================================================
VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct)
{
    if (!pVersionStruct ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    pVersionStruct->pfnGetInstanceProcAddr       = FLM_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr         = FLM_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

} // extern "C"

// ============================================================================
// README — ENV DEĞİŞKENLERİ
// ----------------------------------------------------------------------------
//  FLM_MODE=auto|present|limiter|off   (varsayılan auto)
//     auto     : presentWait varsa PACER, yoksa (fps set ise) LIMITER
//     present  : PACER'ı zorla (presentWait yoksa limiter'a düşer)
//     limiter  : salt FPS limiter (presentWait gerekmez, FLM_TARGET_FPS ister)
//     off      : hiçbir şey yapma (A/B testi taban çizgisi)
//  FLM_TARGET_FPS=<n>          limiter/pacer hedef fps (0 = doğal cadence)
//  FLM_PACE_POINT=present|acquire|both  (varsayılan present) — TEK kapı noktası
//  FLM_PRESENT_LEAD_NS=1000000 flip'ten ne kadar önce present (ns)
//  FLM_SPIN_NS=150000          son N ns pause-spin (0 = tamamen uyku, min CPU)
//  FLM_DRIFT_TOLERANCE_NS=0    0 = otomatik (iv/4)
//  FLM_MFG_MULTIPLIER=0        0 = otomatik algıla, 1-4 = zorla
//  FLM_RT_PRIORITY=0           ölçüm thread SCHED_FIFO önceliği (CAP_SYS_NICE)
//  FLM_MEASURE_CPU=0-3         ölçüm thread affinity
//  FLM_STATS=1                 5 sn'de bir özet log (INFO)
//  FLM_CSV=/tmp/flm.csv        kare bazlı ölçüm dökümü
//  FLM_LOG_LEVEL=DEBUG|INFO|WARN|ERROR
//  FLM_LOG_FILE=/path          log dosyası (varsayılan stderr)
//  SIGUSR1                     dinamik alanları (fps/spin/lead/mode/...) reload
//
//  HIZLI DOĞRULAMA:
//    FLM_MODE=limiter FLM_TARGET_FPS=60 mangohud <oyun>
//      → MangoHud'da düz 60 FPS çizgisi = katman çalışıyor.
//    A/B:  FLM_MODE=off FLM_CSV=/tmp/off.csv   vs
//          FLM_MODE=present FLM_CSV=/tmp/on.csv
//      → on.csv'de interval stddev ve p99 düşük, 1% low yüksek olmalı.
// ============================================================================
