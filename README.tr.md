# FLM — Vulkan Flip Meter / Frame Pacing Katmanı (v2.6)

Linux'ta kare teslimini düzgünleştiren bir Vulkan implicit layer. Özellikle
**donanımsal flip metering'i olmayan GPU'larda** (RTX 40-serisi gibi)
**VRR panel (G-Sync/FreeSync) + frame generation (DLSS-FG / FSR-FG)**
kombinasyonuna odaklanır. `vkQueuePresentKHR` / `vkAcquireNextImageKHR`
çağrılarını yakalar ve moda göre ya FPS'i sabit bir tavana kilitler ya da
present'ları yeniden zamanlayarak karelerin daha eşit aralıklarla kuyruktan
çıkmasını sağlar.

---

## ⚠️ Sorumluluk Reddi (Disclaimer)

Bu proje yapay zeka desteğiyle geliştirilmiştir ve **hiçbir garanti
verilmeksizin, olduğu gibi** sunulur. Çalıştırdığınız her 3D uygulamanın
Vulkan present/acquire yoluna kanca atar; bu da buradaki hataların yalnızca
hedeflenen oyunu değil, **o yolu kullanan herhangi bir oyunu veya uygulamayı**
etkileyebileceği anlamına gelir.

Bu katmanı derleyerek, kurarak veya çalıştırarak şunları kabul etmiş
olursunuz:

- Bazı oyunlarda, motorlarda veya sürücü sürümlerinde **takılma (stutter),
  frame-time bozulması, sürücü kararsızlığı, GPU kilitlenmesi/reset'i,
  compositor bozulmaları veya çökmeler** yaşanabilir.
- Davranış; GPU üreticisine, sürücü sürümüne, DXVK/VKD3D sürümüne ve MFG
  uygulamasına göre değişebilir — bir sistemde iyi çalışan ayar başka bir
  sistemde işe yaramayabilir.
- Canlı ayarlama (`FLM_CONFIG` + `SIGUSR1`) ve ortam değişkenleri doğrulanmamış,
  ileri seviye kullanıcı için düşünülmüş ayarlardır; makul aralıkların dışına
  çıkmak sizin sorumluluğunuzdadır (çoğu iç mekanizmada sınırlanmıştır ama
  her uç durum kapsanmamıştır).
- Güvenmeden önce kendi oyunlarınızda test etmek ve çalışan yapılandırmaların
  yedeğini tutmak sizin sorumluluğunuzdadır.
- Yazarlar; veri kaybı, donanım yıpranması, kaybolan oyun oturumları veya bu
  yazılımın kullanımından doğabilecek başka herhangi bir zarar için
  **hiçbir sorumluluk kabul etmez**.

Bu riskleri değerlendirip kabul etmekte kendinizi rahat hissetmiyorsanız bu
katmanı kullanmayın. Sonuca güvenmeden önce her zaman `FLM_MODE=off` ile bir
temel (baseline) alıp karşılaştırın.

---

## Hangi sorunu çözüyor

VRR ekranlarda, frame generation teknolojileri (DLSS-FG, FSR-FG) gerçek
render edilmiş kareler arasına interpolasyonla üretilmiş kareler ekler.
Donanımsal flip metering'i olmayan GPU'larda bu üretilmiş kareler **eşit
aralıklarla gelmez** — genelde kısa/kısa/kısa/uzun (üç yakın aralıklı kare,
ardından daha uzun bir gerçek kare) şeklinde bir örüntü oluşur. Ortalama FPS
gayet iyi görünse bile panel bunu gözle görülür mikro-jitter olarak gösterir,
çünkü VRR yalnızca kendisine verileni yumuşatır — dengesiz bir varış
örüntüsünü tek başına düzeltemez.

FLM, uygulama ile sürücü arasına girer ve render'ın kendisine dokunmadan
present'ların *ne zaman* gönderildiğini yeniden şekillendirerek bunu düzeltir.

## Size ne kazandırabilir

- **VRR üzerinde frame generation ile daha akıcı algılanan hareket** — ana
  kullanım senaryosu. `FLM_FLOOR_PACING`, üretilmiş kareleri art arda
  bırakmak yerine aralarına mesafe koyarak, aynı ortalama FPS'te jitter
  algısını azaltır.
- **Her sürücüde çalışan güvenilir bir sabit FPS tavanı**
  (`FLM_MODE=limiter`); herhangi bir genişletilmiş sunum özelliği
  gerektirmez — termal/güç sınırları için ya da MFG jitter'ını, jitter'ın
  görünür olduğu noktanın altına kısarak bastırmak için kullanışlıdır.
- **Oyunu yeniden başlatmadan canlı ayar** — bir yapılandırma dosyasına
  yazıp `SIGUSR1` gönderin; ayar denerken yeniden başlatmaya gerek yok.
- **Nesnel önce/sonra ölçüm** — her karenin zamanlamasının CSV'ye
  dökülmesi, iki koşumu (katman kapalı vs açık) karşılaştırıp aralık
  standart sapmasını, p99'u ve %1 düşük değerleri hisle tahmin etmek yerine
  ölçerek kıyaslamanızı sağlar.
- **Otomatik güvenlik davranışı** — oyun GPU-bound olduğunda pacing kendini
  devre dışı bırakır (gerçekten tavana dayanmış bir GPU ile asla
  mücadele etmez), varsayılan olarak FIFO/vsync-kilitli swapchain'lere hiç
  dokunmaz ve küçük yardımcı/overlay pencerelerini otomatik atlar.

## Ne YAPMAZ

- Gerçek kare hızınızı artırmaz veya GPU/CPU render süresini azaltmaz.
- Motor kaynaklı takılmaları (shader derleme, traversal stall'ları, asset
  streaming) düzeltmez — floor-pacing bunları düzgünleştiriyormuş gibi
  davranmak yerine açıkça tespit edip atlar.
- Donanımsal flip metering'i düzgün olan bir GPU'nun yerini tutmaz; bunun
  eksik olduğu GPU'lar için yazılımsal bir yaklaşımdır.

---

## Kısaca nasıl çalışır

Normalde aynı anda yalnızca biri aktif olan iki bağımsız yol vardır:

1. **LIMITER** — `vkQueuePresentKHR`'de saf, yerel-saat tabanlı bir FPS
   tavanı. `presentWait` gerektirmez; her sürücüde çalışır. `FLM_TARGET_FPS`
   verildiğinde kareler her zaman o mutlak temposunda tutulur (deterministik).
2. **PACER** — `VK_KHR_present_wait` gerektirir. Arka plandaki ölçüm thread'i
   gerçek flip zaman damgalarını okur, mevcut tempoyu tahmin eder ve MFG
   otomatik tespit edildiyse üretilmiş kareleri flip aralığına eşit şekilde
   dağıtır. Varsayılan olarak yalnızca MAILBOX/IMMEDIATE swapchain'lerde
   çalışır (FIFO zaten vsync'e kilitlidir — geçersiz kılmak için
   `FLM_PACE_FIFO`'ya bakın).

Pacing, çift gecikme hatalarından kaçınmak için yalnızca tek, yapılandırılabilir
bir gate noktasında (varsayılan `present`) bekler, oyun GPU-bound olduğunda
kendini otomatik devre dışı bırakır ve zaman çizelgesi sapmasını sert bir
sıfırlama yerine kademeli olarak düzeltir. Her düzeltmenin tam, sürüm
numaralandırılmış geçmişi ve gerekçesi için `src/flip_meter.cpp` dosyasının
başındaki yorum bloğuna bakın.

---

## Gereksinimler

- Vulkan loader ve `vk_layer.h` içeren bir Linux kurulumu (Gentoo:
  `media-libs/vulkan-loader`, `media-libs/vulkan-layers`).
- CMake + Ninja veya Make, bir C++17 derleyicisi.
- PACER yolu için sürücünüzde/oyununuzda `VK_KHR_present_wait` desteği
  (isteğe bağlı — LIMITER her yerde çalışır).

## Derleme ve kurulum

```bash
./build.sh                # varsayılan olarak /usr/local'a kurar
./build.sh /usr           # ya da özel bir prefix
FLM_NATIVE_BUILD=ON ./build.sh   # -march=native, yalnızca bu makinede
```

Manifest dosyası (`VkLayer_cpu_flip_meter.json`) CMake tarafından doğru kurulum
yolu ve sürümüyle üretilir ve paylaşımlı kütüphaneyle birlikte kurulur.
Doğrulamak için:

```bash
vulkaninfo --summary | grep -i flip_meter
```

## Hızlı başlangıç

```bash
# Doğrulama — düz bir 60 FPS çizgisi görüyorsanız katman aktiftir:
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=limiter FLM_TARGET_FPS=60 mangohud %command%

# VRR + frame generation, ana kullanım senaryosu:
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=present FLM_FLOOR_PACING=1 \
  FLM_FLOOR_RATIO=850 FLM_CONFIG=/tmp/flm.conf mangohud %command%

# Bunun yerine sabit FPS tavanı (herhangi bir panel tipi):
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=limiter FLM_TARGET_FPS=120 %command%
```

`%command%` Steam başlatma seçenekleri yer tutucusudur; Steam dışında
çalıştırırken bunu kaldırıp oyun binary'sini doğrudan verin.

### `FLM_FLOOR_RATIO`'yu hisse göre ayarlama

| Hissettiğiniz şey | Yapılacak değişiklik |
|---|---|
| Hâlâ mikro-jitter var, MFG'nin ritmi bozuk hissettiriyor | Yükseltin (örn. 850 → 950) |
| Görüntü "yapışkan" hissettiriyor, input gecikmeli hissediliyor | Düşürün (örn. 850 → 700) |
| Ara sıra tek seferlik takılma (sürekli jitter değil) | Bu ayarla ilgili değil — muhtemelen gerçek bir motor hitch'i; hitch guard tarafından zaten atlanıyor |

### Canlı ayarlama (yeniden başlatmadan)

```bash
echo 'FLM_FLOOR_RATIO=900' > /tmp/flm.conf
kill -SIGUSR1 $(pidof <oyun_binary>)
```

Log'da (`FLM_LOG_LEVEL=INFO`) yeniden yüklenen yapılandırma satırı
göründüğünde uygulandığı doğrulanmış olur.

### A/B ölçümü

```bash
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=off     FLM_CSV=/tmp/off.csv %command%
ENABLE_LAYER_cpu_flip_meter=1 FLM_MODE=present FLM_CSV=/tmp/on.csv  %command%
```
İlk 1-2 dakikayı (shader derleme dönemi) analiz dışı bırakın, ardından iki
CSV arasında `interval_ns` standart sapması/p99'unu karşılaştırın.

---

## Tam ortam değişkeni referansı

| Değişken | Amacı |
|---|---|
| `FLM_MODE=auto\|present\|limiter\|off` | `auto` (varsayılan): `presentWait` varsa PACER, yoksa ve hedef FPS ayarlıysa LIMITER. `present`: PACER'ı zorla. `limiter`: saf FPS tavanı. `off`: her şeyi devre dışı bırak (A/B temeli). |
| `FLM_TARGET_FPS=<n>` | LIMITER/PACER için hedef FPS (0-1000, 0 = doğal tempo). Bunu >0 yapmak her zaman sabit tavan (limiter) yolunu devreye sokar. |
| `FLM_PACE_POINT=present\|acquire\|both` | Tek pacing gate'inin hangi çağrıda olduğu (varsayılan `present`). |
| `FLM_PACE_FIFO=1` | PACER/floor-pacing'in FIFO/FIFO_RELAXED swapchain'lerde de çalışmasına izin ver (varsayılan 0/kapalı — FIFO zaten vsync'e kilitlidir). |
| `FLM_FLOOR_PACING=1\|0` | VRR+MFG floor-pacing (varsayılan 1/açık). Bir present'in öncekinden slot genişliğinin belirli bir oranından daha erken çıkmamasını sağlar. |
| `FLM_FLOOR_RATIO=850` | Slot genişliğinin oran/1000 cinsinden tabanı (500-1000). Ana ayar düğmesi — yüksek = daha sıkı/düz, düşük = daha gevşek/az gecikme. Sıcak yeniden yüklenebilir. |
| `FLM_FLOOR_MFG_ADAPT=1` | Tespit edilen MFG çarpanı büyüdükçe `FLOOR_RATIO`'yu otomatik gevşet; yüksek çarpanlar daha fazla tolerans ister. |
| `FLM_FLOOR_MFG_STEP=40` | Çarpan başına oranın ne kadar gevşetileceği (0-200). |
| `FLM_FLOOR_AUTOTUNE=1` | Kapalı döngü oran ayarı: rahatlık payı varken sıkılaştırır, fren belirtilerinde hızla gevşetir. |
| `FLM_PRESENT_LEAD_NS=1000000` | Present'in, tahmin edilen flip'ten ne kadar önce gönderileceği (ns). |
| `FLM_SPIN_NS=150000` | Gate açılmadan önceki son spin-wait penceresi (0 = saf sleep, en düşük CPU kullanımı). |
| `FLM_SPIN_ADAPT=1` | Spin marjını sabit bir değer yerine ölçülen uyanma gecikmesinden otomatik ayarla. |
| `FLM_DRIFT_TOLERANCE_NS=0` | Zaman çizelgesi sapma toleransı (0 = otomatik, aralık/4). |
| `FLM_MFG_MULTIPLIER=0` | 0 = frame generation çarpanını otomatik tespit et, 1-4 = zorla. |
| `FLM_RT_PRIORITY=0` | Ölçüm thread'i için SCHED_FIFO önceliği (`CAP_SYS_NICE` gerektirir). |
| `FLM_MEASURE_CPU=0-3` | Ölçüm thread'ini belirli çekirdeklere sabitle; virgülle ayrılmış aralıkları kabul eder (örn. `0-3,8,10-11`). |
| `FLM_STATS=1` / `FLM_STATS_INTERVAL=5` | Periyodik özet log (sayım, ortalama, p99, maksimum, fake/hitch sayıları, MFG çarpanı, pacing durumu). |
| `FLM_CSV=/tmp/flm.csv` | Çevrimdışı analiz için kare bazlı CSV dökümü (swapchain yeniden oluşturmadan etkilenmez; kesip baştan yazmak yerine ekler). |
| `FLM_CONFIG=/tmp/flm.conf` | Canlı yapılandırma dosyası (`KEY=VALUE`, `#` yorumları); yeniden başlatmadan yeniden yüklemek için `SIGUSR1` ile birlikte kullanın. |
| `FLM_LOG_LEVEL=DEBUG\|INFO\|WARN\|ERROR` | Log ayrıntı düzeyi. |
| `FLM_LOG_FILE=/path` | Log hedefi (varsayılan stderr). |
| `SIGUSR1` | Çalışma zamanında `FLM_CONFIG` dosyasını yeniden oku. |

---

## Sorun giderme

1. **Katmanın gerçekten yüklendiğini doğrulayın**: `FLM_LOG_LEVEL=INFO
   FLM_LOG_FILE=/tmp/flm.log` ile çalıştırın ve `Config: mode=...`
   satırını arayın.
2. **`presentWait` desteğini kontrol edin**: log desteklenmediğini
   bildiriyorsa, yalnızca LIMITER kullanılabilir — PACER ve floor-pacing
   buna ihtiyaç duyar.
3. **FIFO olup olmadığını kontrol edin**: `FLM_MODE=present` altında PACER
   etkisizmiş gibi görünüyorsa, swapchain FIFO (vsync-kilitli) olabilir; bu,
   `FLM_PACE_FIFO=1` ayarlamadığınız sürece tasarım gereğidir.
4. **Biraz zaman tanıyın**: ilk ~30 kare pacing'siz bir ısınma penceresidir.
5. **Tahmin etmeyin, ölçün**: `FLM_CSV` kullanın ve aynı sahnede
   `FLM_MODE=off` temeliyle stddev/p99'u karşılaştırın.

## Lisans

`LICENSE` dosyasına bakın.
