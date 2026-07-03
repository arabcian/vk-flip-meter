#!/bin/bash
# build.sh — build and install vk_flip_meter layer with heavy optimizations
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_PREFIX="${1:-/usr/local}"

echo "==> Building vk_flip_meter"
echo "    Prefix: $INSTALL_PREFIX"
echo ""

# Check dependencies on Gentoo
check_dep() {
    if ! pkg-config --exists "$1" 2>/dev/null; then
        echo "ERROR: $1 not found. On Gentoo: sudo emerge -av $2"
        exit 1
    fi
}

# Check Vulkan headers
if [ ! -f /usr/include/vulkan/vulkan.h ] && \
   [ ! -f /usr/local/include/vulkan/vulkan.h ]; then
    echo "ERROR: Vulkan headers not found."
    echo "       On Gentoo: sudo emerge -av media-libs/vulkan-loader"
    exit 1
fi

# Check vk_layer.h specifically
if ! find /usr/include /usr/local/include -name "vk_layer.h" 2>/dev/null | grep -q .; then
    echo "ERROR: vk_layer.h not found."
    echo "       On Gentoo: sudo emerge -av media-libs/vulkan-layers"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Temiz bir derleme ve LTO sembollerinin çakışmaması için eski cache'i temizliyoruz
echo "==> Clearing old CMake cache for optimization flags..."
rm -rf CMakeCache.txt CMakeFiles/

echo "==> Injecting Native CPU flags and Aggressive Optimizations..."
# -O3, -march=native, LTO, no-exceptions, visibility hiding ve linker optimizasyonları
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_INSTALL_LIBDIR="lib64" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native -flto=auto -fno-plt -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden" \
    -DCMAKE_SHARED_LINKER_FLAGS="-flto=auto -Wl,-O3 -Wl,--sort-common -Wl,--as-needed -Wl,-z,now -Wl,-z,relro" \
    -G Ninja

ninja -j$(nproc)

echo ""
echo "==> Installing (may need sudo)"
sudo ninja install

echo ""
echo "==> Updating manifest library path"
MANIFEST="/usr/local/share/vulkan/implicit_layer.d/VkLayer_cpu_flip_meter.json"
LIB_PATH="$INSTALL_PREFIX/lib64/libvk_flip_meter.so"
sudo sed -i "s|/usr/local/lib64/libvk_flip_meter.so|$LIB_PATH|g" "$MANIFEST"
echo ""
echo "==> Done. Verify install:"
echo "    vulkaninfo --summary | grep flip_meter"
echo ""
echo "Usage:"
echo "  # Standard FG (1x):"
echo "  ENABLE_LAYER_cpu_flip_meter=1 %command%"
echo ""
echo "  # MFG 4x:"
echo "  ENABLE_LAYER_cpu_flip_meter=1 FLM_MFG_MULTIPLIER=4 %command%"
echo ""
echo "  # MFG 4x with target FPS cap + verbose:"
echo "  ENABLE_LAYER_cpu_flip_meter=1 FLM_MFG_MULTIPLIER=4 FLM_TARGET_FPS=60 FLM_VERBOSE=1 %command%"
echo ""
echo "  # Precision Tuning (Drift Tolerance & CPU Spin):"
echo "  ENABLE_LAYER_cpu_flip_meter=1 FLM_DRIFT_TOLERANCE_NS=1500000 FLM_SPIN_THRESHOLD_NS=150000 %command%"
